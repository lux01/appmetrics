/*******************************************************************************
 * Copyright 2014, 2015 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *******************************************************************************/

#include "AgentExtensions.h"
#include "Typesdef.h"
#include "v8.h"
#include "v8-profiler.h"
#include "uv.h"
#include "nan.h"
//#include "node_version.h"
#include <cstring>
#include <string>
#include <sstream>
#if defined(_WINDOWS)
#include <ctime>
#else
#include <sys/time.h>
#endif

#define DEFAULT_CAPACITY 10240

#if defined(_WINDOWS)
#define NODEPROFPLUGIN_DECL __declspec(dllexport)	/* required for DLLs to export the plugin functions */
#else
#define NODEPROFPLUGIN_DECL
#endif

#if defined(_WINDOWS)
static unsigned long long GetRealTime() {
	SYSTEMTIME st;
	GetSystemTime(&st);
	return std::time(NULL) * 1000 + st.wMilliseconds;
}
#else
static unsigned long long GetRealTime() {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long long)(tv.tv_sec) * 1000 +
	       (unsigned long long)(tv.tv_usec) / 1000;
}
#endif

#define PROFILING_INTERVAL 5000

namespace plugin {
	// NOTE(tunniclm): only access these variables from the V8/Node/uv thread
	agentCoreFunctions api;
	uint32 provid = 0;	
	bool enabled = false;
	bool profiling = false;
	uv_timer_t *timer;
}

using namespace v8;
using namespace ibmras::common::logging;

static char* NewCString(const std::string& s) {
	char *result = new char[s.length() + 1];
	std::strcpy(result, s.c_str());
	return result;
}

// NOTE(tunniclm): Must be called from the V8/Node/uv thread
//                 since it calls V8 APIs
static bool ExtractV8String(const Handle<String> v8string, char **cstring) {
	*cstring = new char[v8string->Length() + 1];
	if (*cstring == NULL) return false;
	v8string->WriteUtf8(*cstring);
	return true;
}

static void ConstructNodeData(const CpuProfileNode *node, int id, int parentId, std::stringstream &result) {
	int line = node->GetLineNumber();
	double selfSamples = 0;

	#if NODE_VERSION_AT_LEAST(0, 11, 0) // > v0.11+
		selfSamples = node->GetHitCount();
	#else
		selfSamples = node->GetSelfSamplesCount();
	#endif

	char *function, *script;
	if (!ExtractV8String(node->GetFunctionName(), &function)) {
		return;
	}
	if (!ExtractV8String(node->GetScriptResourceName(), &script)) {
		delete[] function;
		return;
	}

	result << "NodeProfData,Node," << id << ',' << parentId << ',';
	result << script << ',' << function << ',' << line << ',' << selfSamples << '\n';

	// clean up
	delete[] script;
	delete[] function;
}

typedef void visit_callback(const CpuProfileNode *, int, int, std::stringstream &result);
static void visit(const CpuProfileNode *current, visit_callback *cb, int parentId, std::stringstream &result) {
	static int nextid = 1;
	int id = nextid++;
	cb(current, id, parentId, result);

	int children = current->GetChildrenCount();
	for (int i=0; i<children; i++) {
		visit(current->GetChild(i), cb, id, result);
	}
}

static char * ConstructData(const CpuProfile *profile) {
	const CpuProfileNode *topRoot = profile->GetTopDownRoot();

	std::stringstream result;
	result << "NodeProfData,Start," << GetRealTime() << '\n';
	visit(topRoot, ConstructNodeData, 0, result);
	result << "NodeProfData,End" << '\n';
	return NewCString(result.str());
}

#if NODE_VERSION_AT_LEAST(0, 11, 0) // > v0.11+

// NOTE(tunniclm): Must be called from the V8/Node/uv thread
//                 since it calls V8 APIs
static Isolate* GetIsolate() {
	Isolate *isolate = v8::Isolate::GetCurrent();
	if (isolate == NULL) {
		plugin::api.logMessage(debug, "[profiling_node] No V8 Isolate found");
	}
	return isolate;
}

// NOTE(tunniclm): Must be called from the V8/Node/uv thread
//                 since it calls V8 APIs
static CpuProfiler* GetCpuProfiler(Isolate *isolate) {
	CpuProfiler *cpu = isolate->GetCpuProfiler();
	if (cpu == NULL) {
		plugin::api.logMessage(debug, "[profiling_node] No CpuProfiler found");
	}
	return cpu;
}

#endif

// NOTE(tunniclm): Must be called from the V8/Node/uv thread
//                 since it calls V8 APIs
static void StartTheProfiler() {
#if NODE_VERSION_AT_LEAST(0, 11, 0) // > v0.11+
	Isolate *isolate = GetIsolate();
	if (isolate == NULL) return;
	
	CpuProfiler *cpu = GetCpuProfiler(isolate);
	if (cpu == NULL) return;

	cpu->StartProfiling(NanNew<String>("NodeProfPlugin"), false);
#else
	CpuProfiler::StartProfiling(NanNew<String>("NodeProfPlugin"));
#endif
}

// NOTE(tunniclm): Must be called from the V8/Node/uv thread
//                 since it calls V8 APIs
static const CpuProfile* StopTheProfiler() {
#if NODE_VERSION_AT_LEAST(0, 11, 0) // > v0.11+
	Isolate *isolate = GetIsolate();
	if (isolate == NULL) return NULL;

	CpuProfiler *cpu = GetCpuProfiler(isolate);
	if (cpu == NULL) return NULL;
	
	return cpu->StopProfiling(NanNew<String>("NodeProfPlugin"));
#else
	return CpuProfiler::StopProfiling(NanNew<String>("NodeProfPlugin"));
#endif
}

static void ReleaseProfile(const CpuProfile *profile) {
	if (profile != NULL) {
		const_cast<CpuProfile *>(profile)->Delete();
	}
}

// NOTE(tunniclm): Must be called from the V8/Node/uv thread
//                 since it calls V8 APIs
//                 and accesses non thread-safe fields
#if NODE_VERSION_AT_LEAST(0, 11, 0) // > v0.11+
void OnGatherDataOnV8Thread(uv_timer_s *data) {
#else
void OnGatherDataOnV8Thread(uv_timer_s *data, int status) {
#endif

	// Check if we just got disabled and the profiler
	// isn't running
	if (!plugin::enabled) return;
	
	NanScope();
	
	// Get profile
	const CpuProfile *profile = StopTheProfiler();

	if (profile != NULL) {
		char *serialisedProfile = ConstructData(profile);
		ReleaseProfile(profile);
		StartTheProfiler();
		if (serialisedProfile != NULL) {
			// Send data to agent
			monitordata data;
			data.persistent = false;
			data.provID = plugin::provid;
			data.sourceID = 0;
			data.size = static_cast<uint32>(strlen(serialisedProfile));
			data.data = serialisedProfile;
			plugin::api.agentPushData(&data);
			
			delete[] serialisedProfile;
		} else {
			plugin::api.logMessage(debug, "[profiling_node] Failed to serialise method profile"); // CHECK(tunniclm): Should this be a warning?
		}
	} else {
		plugin::api.logMessage(debug, "[profiling_node] No method profile found"); // CHECK(tunniclm): Should this be a warning?
		StartTheProfiler();
	}
}

pushsource* createPushSource(uint32 srcid, const char* name) {
	pushsource *src = new pushsource();
	src->header.name = name;
	std::string desc("Description for ");
	desc.append(name);
	src->header.description = NewCString(desc);
	src->header.sourceID = srcid;
	src->next = NULL;
	src->header.capacity = DEFAULT_CAPACITY;
	return src;
}

static void publishEnabled() {
	std::string sourceName = "profiling_node";
	std::string msg = sourceName + "_subsystem=";
	if (plugin::enabled) {
		msg += "on";
	} else {
		msg += "off";
	}

	std::stringstream logMsg;
	logMsg << "[profiling_node] Sending config message [" << msg << "]";
	plugin::api.logMessage(debug,  logMsg.str().c_str());
	
	plugin::api.agentSendMessage(("configuration/" + sourceName).c_str(), msg.length(),
								  (void*) msg.c_str());
} 

static void cleanupHandle(uv_handle_t *handle) {
	delete handle;
}

// NOTE(tunniclm): Must be called from the V8/Node/uv thread
//                 since it calls non thread-safe uv APIs
//                 and accesses non thread-safe fields
#if NODE_VERSION_AT_LEAST(0, 11, 0) // > v0.11+
static void enableOnV8Thread(uv_async_t *async) {
#else
static void enableOnV8Thread(uv_async_t *async, int status) {
#endif
	if (plugin::enabled) return;
	plugin::enabled = true;
	plugin::api.logMessage(debug, "[profiling_node] Publishing config");
    publishEnabled();
	
	StartTheProfiler();

	uv_timer_start(plugin::timer, OnGatherDataOnV8Thread, PROFILING_INTERVAL, PROFILING_INTERVAL);
		
	uv_close((uv_handle_t*) async, cleanupHandle);
}

// NOTE(tunniclm): Must be called from the V8/Node/uv thread
//                 since it calls non thread-safe uv APIs
//                 and accesses non thread-safe fields
#if NODE_VERSION_AT_LEAST(0, 11, 0) // > v0.11+
static void disableOnV8Thread(uv_async_t *async) {
#else
static void disableOnV8Thread(uv_async_t *async, int status) {
#endif
	if (!plugin::enabled) return;
	plugin::enabled = false;
	plugin::api.logMessage(debug, "[profiling_node] Publishing config");
    publishEnabled();

	uv_timer_stop(plugin::timer);
	
	const CpuProfile *profile = StopTheProfiler();
	ReleaseProfile(profile);

	uv_close((uv_handle_t*) async, cleanupHandle);
}

// NOTE(tunniclm): Don't access plugin::enabled or plugin::profiling in here
//                 since this function may not be running on the V8/Node/uv 
//                 thread. uv_async_send() is thread-safe.
void setEnabled(bool value) {
	if (value) {
		plugin::api.logMessage(fine, "[profiling_node] Enabling");
		uv_async_t *async = new uv_async_t;
		uv_async_init(uv_default_loop(), async, enableOnV8Thread);
		uv_async_send(async); // close and cleanup in call back
	} else {
		plugin::api.logMessage(fine, "[profiling_node] Disabling");
		uv_async_t *async = new uv_async_t;
		uv_async_init(uv_default_loop(), async, disableOnV8Thread);
		uv_async_send(async); // close and cleanup in call back
	}
}

extern "C" {
	// NOTE(tunniclm): Must be called from the V8/Node/uv thread as
	//                 it accesses non thread-safe fields
	NODEPROFPLUGIN_DECL pushsource* ibmras_monitoring_registerPushSource(agentCoreFunctions api, uint32 provID) {
		plugin::api = api;
	
		std::string enabledProp(plugin::api.getProperty("com.ibm.diagnostics.healthcenter.data.profiling"));
		plugin::enabled = (enabledProp == "on");
	
		plugin::api.logMessage(debug, "[profiling_node] Registering push sources");
		pushsource *head = createPushSource(0, "profiling_node");
		plugin::provid = provID;
		return head;
	}
	
	// NOTE(tunniclm): Must be called from the V8/Node/uv thread as
	//                 it accesses non thread-safe fields
	NODEPROFPLUGIN_DECL int ibmras_monitoring_plugin_init(const char* properties) {
		// NOTE(tunniclm): We don't have the agentCoreFunctions yet, so we can't do any init that requires
		//                 calling into the API (eg getting properties.)	
		return 0;
	}
	
	// NOTE(tunniclm): Must be called from the V8/Node/uv thread
	//                 since it calls non thread-safe V8 APIs and
	//                 uv APIs and accesses non thread-safe fields
	NODEPROFPLUGIN_DECL int ibmras_monitoring_plugin_start() {
		if (plugin::enabled) {	
			plugin::api.logMessage(fine, "[profiling_node] Starting enabled");
		} else {
			plugin::api.logMessage(fine, "[profiling_node] Starting disabled");
		}
	
		plugin::api.logMessage(debug, "[profiling_node] Publishing config");
		publishEnabled();	
	
		plugin::timer = new uv_timer_t;
		uv_timer_init(uv_default_loop(), plugin::timer);
		uv_unref((uv_handle_t*) plugin::timer); // don't prevent event loop exit
		
		if (plugin::enabled) {	
			plugin::api.logMessage(debug, "[profiling_node] Start profiling");
			StartTheProfiler();
		
			plugin::api.logMessage(debug, "[profiling_node] Starting timer");
			uv_timer_start(plugin::timer, OnGatherDataOnV8Thread, PROFILING_INTERVAL, PROFILING_INTERVAL);
		}
	
		return 0;
	}
	
	NODEPROFPLUGIN_DECL int ibmras_monitoring_plugin_stop() {
		plugin::api.logMessage(fine, "[profiling_node] Stopping");
	
		if (plugin::enabled) {
			plugin::enabled = false;
	
			uv_timer_stop(plugin::timer);
			uv_close((uv_handle_t*) plugin::timer, cleanupHandle);
		
			const CpuProfile *profile = StopTheProfiler();
			ReleaseProfile(profile);
		}
	
		return 0;
	}
	
	NODEPROFPLUGIN_DECL void ibmras_monitoring_receiveMessage(const char *id, uint32 size, void *data) {
		std::string idstring(id);
	
		if (idstring == "profiling_node") {
			//std::stringstream ss;
			//ss << "Received message with id [" << idstring << "], size [" << size << "]";
			//plugin::api.logMessage(debug, ss.str().c_str());
			
			std::string message((const char*) data, size);
			//if (size > 0) {
			//	std::string msg = "Message content [" + message + "]";
			//	plugin::api.logMessage(debug, msg.c_str());
			//}
			std::size_t found = message.find(',');
			std::string command = message.substr(0, found);
			std::string rest = message.substr(found + 1);
			
			if (rest == "profiling_node_subsystem") {
				bool enabled = (command == "on");
				//std::string msg = "Setting [" + rest + "] to " + (enabled ? "enabled" : "disabled");
				//plugin::api.logMessage(debug, msg.c_str());
				setEnabled(enabled);
			}
		} 
	}
	
	NODEPROFPLUGIN_DECL const char* ibmras_monitoring_getVersion() {
		return "2.0";
	}
}