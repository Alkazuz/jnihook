/*
 *  -----------------------------------
 * |         JNIHook - by rdbo         |
 * |      Java VM Hooking Library      |
 *  -----------------------------------
 */

/*
 * Copyright (C) 2023    Rdbo
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License version 3
 * as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 * 
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <jnihook.h>
#include <unordered_map>
#include <string>
#include <vector>
#include <cstdint>
#include "classfile.hpp"

#include <iostream> // TODO: Remove

typedef struct method_info_t {
	std::string name;
	std::string signature;
} method_info_t;

typedef struct hook_info_t {
	method_info_t method_info;
	void *native_hook_method;
} hook_info_t;

// TODO: Make the following globals specific to a jnihook_t without breaking C compat
static std::unordered_map<std::string, std::vector<hook_info_t>> g_hooks;
static std::unordered_map<std::string, std::unique_ptr<ClassFile>> g_class_file_cache;

static std::string
get_class_signature(jvmtiEnv *jvmti, jclass clazz)
{
	char *sig;
	
	if (jvmti->GetClassSignature(clazz, &sig, NULL) != JVMTI_ERROR_NONE) {
		return "";
	}

	std::string signature = std::string(sig, &sig[strlen(sig)]);

	jvmti->Deallocate(reinterpret_cast<unsigned char *>(sig));

	return signature;
}

static std::string
get_class_name(JNIEnv *env, jclass clazz)
{
	jclass klass = env->FindClass("java/lang/Class");
	if (!klass)
		return "";

	jmethodID getName_method = env->GetMethodID(klass, "getName", "()Ljava/lang/String;");
	if (!getName_method)
		return "";

	jstring name_obj = reinterpret_cast<jstring>(env->CallObjectMethod(clazz, getName_method));
	if (!name_obj)
		return "";

	const char *c_name = env->GetStringUTFChars(name_obj, 0);
	if (!c_name)
		return "";

	std::string name = std::string(c_name, &c_name[strlen(c_name)]);

	env->ReleaseStringUTFChars(name_obj, c_name);

	// Replace dots with slashes to match contents of ClassFile
	for (size_t i = 0; i < name.length(); ++i) {
		if (name[i] == '.')
			name[i] = '/';
	}
	
	return name;
}

static std::unique_ptr<method_info_t>
get_method_info(jvmtiEnv *jvmti, jmethodID method)
{
	char *name;
	char *sig;
	
	if (jvmti->GetMethodName(method, &name, &sig, NULL) != JVMTI_ERROR_NONE)
		return nullptr;

	std::string name_str(name, &name[strlen(name)]);
	std::string signature_str(sig, &sig[strlen(sig)]);

	jvmti->Deallocate(reinterpret_cast<unsigned char *>(name));
	jvmti->Deallocate(reinterpret_cast<unsigned char *>(sig));

	return std::make_unique<method_info_t>(method_info_t { name_str, signature_str });
}

void JNICALL JNIHook_ClassFileLoadHook(jvmtiEnv *jvmti_env,
				       JNIEnv* jni_env,
				       jclass class_being_redefined,
				       jobject loader,
				       const char* name,
				       jobject protection_domain,
				       jint class_data_len,
				       const unsigned char* class_data,
				       jint* new_class_data_len,
				       unsigned char** new_class_data)
{
	auto class_name = get_class_name(jni_env, class_being_redefined);

	// If the cache is not hooked, or it is already cached, no point in caching again
	if (class_name == "" || g_hooks.find(class_name) == g_hooks.end() ||
	    g_class_file_cache.find(class_name) != g_class_file_cache.end()) {
		return;
	}

	// Cache parsed classfile
	auto cf = ClassFile::load(class_data);
	if (!cf)
		return;

	g_class_file_cache[class_name] = std::move(cf);

	return;
}

JNIHOOK_API jnihook_result_t JNIHOOK_CALL
JNIHook_Init(JNIEnv *env, jnihook_t *jnihook)
{
	JavaVM *jvm;
	jvmtiEnv *jvmti;
	jvmtiCapabilities capabilities = { 0 };
	jvmtiEventCallbacks callbacks = {};

	if (env->GetJavaVM(&jvm) != JNI_OK) {
		return JNIHOOK_ERR_GET_JVM;
	}

	if (jvm->GetEnv(reinterpret_cast<void **>(&jvmti), JVMTI_VERSION_1_2) != JNI_OK) {
		return JNIHOOK_ERR_GET_JVMTI;
	}

	capabilities.can_redefine_classes = 1;
	capabilities.can_redefine_any_class = 1;
	capabilities.can_retransform_classes = 1;
	capabilities.can_retransform_any_class = 1;
	if (jvmti->GetPotentialCapabilities(&capabilities) != JVMTI_ERROR_NONE) {
		return JNIHOOK_ERR_ADD_JVMTI_CAPS;
	}

	if (jvmti->AddCapabilities(&capabilities) != JVMTI_ERROR_NONE) {
		return JNIHOOK_ERR_ADD_JVMTI_CAPS;
	}

	callbacks.ClassFileLoadHook = JNIHook_ClassFileLoadHook;
	if (jvmti->SetEventCallbacks(&callbacks, sizeof(callbacks)) != JVMTI_ERROR_NONE) {
		return JNIHOOK_ERR_SETUP_CLASS_FILE_LOAD_HOOK;
	}

	if (jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL) != JVMTI_ERROR_NONE) {
		return JNIHOOK_ERR_SETUP_CLASS_FILE_LOAD_HOOK;
	}

	jnihook->jvm = jvm;
	jnihook->env = env;
	jnihook->jvmti = jvmti;

	return JNIHOOK_OK;
}

JNIHOOK_API jint JNIHOOK_CALL
JNIHook_Attach(jnihook_t *jnihook, jmethodID method, void *native_hook_method)
{
	jclass clazz;
	std::string clazz_name;
	hook_info_t hook_info;
	jvmtiClassDefinition class_definition;

	if (jnihook->jvmti->GetMethodDeclaringClass(method, &clazz) != JVMTI_ERROR_NONE) {
		return JNIHOOK_ERR_JVMTI_OPERATION;
	}

	clazz_name = get_class_name(jnihook->env, clazz);
	if (clazz_name.length() == 0) {
		return JNIHOOK_ERR_JNI_OPERATION;
	}

	auto method_info = get_method_info(jnihook->jvmti, method);
	if (!method_info) {
		return JNIHOOK_ERR_JVMTI_OPERATION;
	}

	hook_info.method_info = *method_info;
	hook_info.native_hook_method = native_hook_method;

	g_hooks[clazz_name].push_back(hook_info);

	// Force caching of the class being hooked
	if (g_class_file_cache.find(clazz_name) == g_class_file_cache.end()) {
		if (jnihook->jvmti->RetransformClasses(1, &clazz) != JVMTI_ERROR_NONE)
			return JNIHOOK_ERR_JVMTI_OPERATION;
	}

	if (g_class_file_cache.find(clazz_name) == g_class_file_cache.end()) {
		return JNIHOOK_ERR_CLASSFILE_CACHE;
	}

	auto cf = *g_class_file_cache[clazz_name];

	auto constant_pool = cf.get_constant_pool();

	// Patch class file
	// NOTE: The `methods` attribute only has the methods defined by the main class of this ClassFile
	//       Method references are not included here
	//       If the source file has more than one class, they are compiled as separate ClassFiles
	for (auto &method : cf.get_methods()) {
		auto name_ci = reinterpret_cast<CONSTANT_Utf8_info *>(
			cf.get_constant_pool_item(method.name_index).bytes.data()
		);

		auto descriptor_ci = reinterpret_cast<CONSTANT_Utf8_info *>(
			cf.get_constant_pool_item(method.descriptor_index).bytes.data()
		);

		auto name = std::string(name_ci->bytes, &name_ci->bytes[name_ci->length]);
		auto descriptor = std::string(descriptor_ci->bytes, &descriptor_ci->bytes[descriptor_ci->length]);

		// Check if the current method is a method that should be hooked
		// TODO: Use hashmap for faster lookup
		bool should_hook = false;
		for (auto &hk_info : g_hooks[clazz_name]) {
			auto &minfo = hk_info.method_info;
			if (minfo.name == name && minfo.signature == descriptor) {
				should_hook = true;
				break;
			}
		}
		if (!should_hook)
			continue;

		// Set method to native
		method.access_flags |= ACC_NATIVE;

		// Remove "Code" attribute
		for (size_t i = 0; i < method.attributes.size(); ++i) {
			auto attr = method.attributes[i];
			auto attr_name_ci = reinterpret_cast<CONSTANT_Utf8_info *>(
				cf.get_constant_pool_item(attr.attribute_name_index).bytes.data()
			);
			auto attr_name = std::string(attr_name_ci->bytes, &attr_name_ci->bytes[attr_name_ci->length]);
			if (attr_name == "Code") {
				method.attributes.erase(method.attributes.begin() + i);
				break;
			}
		}

		break;
	}

	// Redefine class with modified ClassFile
	auto cf_bytes = cf.bytes();
	class_definition.klass = clazz;
	class_definition.class_byte_count = cf_bytes.size();
	class_definition.class_bytes = cf_bytes.data();
	if (jnihook->jvmti->RedefineClasses(1, &class_definition) != JVMTI_ERROR_NONE)
		return JNIHOOK_ERR_JVMTI_OPERATION;

	// Register native methods for JVM lookup
	std::vector<JNINativeMethod> native_methods;
	for (auto &hk_info : g_hooks[clazz_name]) {
		JNINativeMethod native_method;
		native_method.name = const_cast<char *>(hk_info.method_info.name.c_str());
		native_method.signature = const_cast<char *>(hk_info.method_info.signature.c_str());
		native_method.fnPtr = hk_info.native_hook_method;
		native_methods.push_back(native_method);
	}
	if (jnihook->env->RegisterNatives(clazz, native_methods.data(), native_methods.size()) < 0) {
		return JNIHOOK_ERR_JNI_OPERATION;
	}

	return JNIHOOK_OK;
}

JNIHOOK_API jint JNIHOOK_CALL JNIHook_Detach(jnihook_t *jnihook, jmethodID method);


JNIHOOK_API void JNIHOOK_CALL JNIHook_Shutdown(jnihook_t *jnihook)
{
	jvmtiEventCallbacks callbacks = {};

	jnihook->jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);
	jnihook->jvmti->SetEventCallbacks(&callbacks, sizeof(callbacks));

	memset(jnihook, 0x0, sizeof(*jnihook));

	return;
}
