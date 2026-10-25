/*
 * Quartix - synthetic JNI: a stand-in JavaVM / JNIEnv for the engine.
 * Copyright (C) 2026 Quartix contributors. GPLv3.
 *
 * This build of libroblox.so is a GameActivity app with a large Java layer: it
 * exports ~572 JNI native methods and, at runtime, calls *back* into Java
 * classes (com.roblox.engine.jni.*, com.roblox.client.*, com.google.android-
 * gamesdk.GameActivity) through the JNIEnv function table. There is no Dalvik/
 * ART here, so Quartix supplies a synthetic JavaVM and JNIEnv: the standard JNI
 * 1.6 function tables, wired so the engine can be entered (JNI_OnLoad) and so
 * every call the engine makes into "Java" is intercepted.
 *
 * This first slice is the foundation and a discovery tool: the resolution calls
 * (FindClass / GetMethodID / RegisterNatives / refs / exceptions / strings) have
 * real minimal behaviour, and every other JNIEnv slot logs itself, so a run
 * against the real engine prints the exact, ordered list of what it needs from
 * the Java side - which is what the next slices implement.
 *
 * ABI: the engine calls every table entry System V style, so each thunk here is
 * sysv_abi. The function tables are laid out exactly as JNI 1.6 defines them
 * (verified against jni.h); the engine indexes them by offset, so order is law.
 * The table fields are plain void* and the engine calls through its own jni.h
 * prototypes - this file only has to place an ABI-compatible pointer at each
 * offset.
 */
#include "quartix.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define SYSV __attribute__((sysv_abi))

/* --- JNI scalar types (the stable Khronos/JNI ABI) --- */
typedef int32_t jint;
typedef int64_t jlong;
typedef int8_t jbyte;
typedef uint8_t jboolean;
typedef uint16_t jchar;
typedef int16_t jshort;
typedef float jfloat;
typedef double jdouble;
typedef jint jsize;

#define JNI_OK 0
#define JNI_FALSE 0
#define JNI_TRUE 1
#define JNI_VERSION_1_6 0x00010006

/* The engine calls through its own jni.h prototypes; our table just needs an
 * ABI-compatible pointer at each JNI 1.6 offset, so every field is void*. */
struct JNINativeInterface_ {
    void *reserved0, *reserved1, *reserved2, *reserved3;
    void *GetVersion;
    void *DefineClass;
    void *FindClass;
    void *FromReflectedMethod;
    void *FromReflectedField;
    void *ToReflectedMethod;
    void *GetSuperclass;
    void *IsAssignableFrom;
    void *ToReflectedField;
    void *Throw;
    void *ThrowNew;
    void *ExceptionOccurred;
    void *ExceptionDescribe;
    void *ExceptionClear;
    void *FatalError;
    void *PushLocalFrame;
    void *PopLocalFrame;
    void *NewGlobalRef;
    void *DeleteGlobalRef;
    void *DeleteLocalRef;
    void *IsSameObject;
    void *NewLocalRef;
    void *EnsureLocalCapacity;
    void *AllocObject;
    void *NewObject;
    void *NewObjectV;
    void *NewObjectA;
    void *GetObjectClass;
    void *IsInstanceOf;
    void *GetMethodID;
    void *CallObjectMethod;
    void *CallObjectMethodV;
    void *CallObjectMethodA;
    void *CallBooleanMethod;
    void *CallBooleanMethodV;
    void *CallBooleanMethodA;
    void *CallByteMethod;
    void *CallByteMethodV;
    void *CallByteMethodA;
    void *CallCharMethod;
    void *CallCharMethodV;
    void *CallCharMethodA;
    void *CallShortMethod;
    void *CallShortMethodV;
    void *CallShortMethodA;
    void *CallIntMethod;
    void *CallIntMethodV;
    void *CallIntMethodA;
    void *CallLongMethod;
    void *CallLongMethodV;
    void *CallLongMethodA;
    void *CallFloatMethod;
    void *CallFloatMethodV;
    void *CallFloatMethodA;
    void *CallDoubleMethod;
    void *CallDoubleMethodV;
    void *CallDoubleMethodA;
    void *CallVoidMethod;
    void *CallVoidMethodV;
    void *CallVoidMethodA;
    void *CallNonvirtualObjectMethod;
    void *CallNonvirtualObjectMethodV;
    void *CallNonvirtualObjectMethodA;
    void *CallNonvirtualBooleanMethod;
    void *CallNonvirtualBooleanMethodV;
    void *CallNonvirtualBooleanMethodA;
    void *CallNonvirtualByteMethod;
    void *CallNonvirtualByteMethodV;
    void *CallNonvirtualByteMethodA;
    void *CallNonvirtualCharMethod;
    void *CallNonvirtualCharMethodV;
    void *CallNonvirtualCharMethodA;
    void *CallNonvirtualShortMethod;
    void *CallNonvirtualShortMethodV;
    void *CallNonvirtualShortMethodA;
    void *CallNonvirtualIntMethod;
    void *CallNonvirtualIntMethodV;
    void *CallNonvirtualIntMethodA;
    void *CallNonvirtualLongMethod;
    void *CallNonvirtualLongMethodV;
    void *CallNonvirtualLongMethodA;
    void *CallNonvirtualFloatMethod;
    void *CallNonvirtualFloatMethodV;
    void *CallNonvirtualFloatMethodA;
    void *CallNonvirtualDoubleMethod;
    void *CallNonvirtualDoubleMethodV;
    void *CallNonvirtualDoubleMethodA;
    void *CallNonvirtualVoidMethod;
    void *CallNonvirtualVoidMethodV;
    void *CallNonvirtualVoidMethodA;
    void *GetFieldID;
    void *GetObjectField;
    void *GetBooleanField;
    void *GetByteField;
    void *GetCharField;
    void *GetShortField;
    void *GetIntField;
    void *GetLongField;
    void *GetFloatField;
    void *GetDoubleField;
    void *SetObjectField;
    void *SetBooleanField;
    void *SetByteField;
    void *SetCharField;
    void *SetShortField;
    void *SetIntField;
    void *SetLongField;
    void *SetFloatField;
    void *SetDoubleField;
    void *GetStaticMethodID;
    void *CallStaticObjectMethod;
    void *CallStaticObjectMethodV;
    void *CallStaticObjectMethodA;
    void *CallStaticBooleanMethod;
    void *CallStaticBooleanMethodV;
    void *CallStaticBooleanMethodA;
    void *CallStaticByteMethod;
    void *CallStaticByteMethodV;
    void *CallStaticByteMethodA;
    void *CallStaticCharMethod;
    void *CallStaticCharMethodV;
    void *CallStaticCharMethodA;
    void *CallStaticShortMethod;
    void *CallStaticShortMethodV;
    void *CallStaticShortMethodA;
    void *CallStaticIntMethod;
    void *CallStaticIntMethodV;
    void *CallStaticIntMethodA;
    void *CallStaticLongMethod;
    void *CallStaticLongMethodV;
    void *CallStaticLongMethodA;
    void *CallStaticFloatMethod;
    void *CallStaticFloatMethodV;
    void *CallStaticFloatMethodA;
    void *CallStaticDoubleMethod;
    void *CallStaticDoubleMethodV;
    void *CallStaticDoubleMethodA;
    void *CallStaticVoidMethod;
    void *CallStaticVoidMethodV;
    void *CallStaticVoidMethodA;
    void *GetStaticFieldID;
    void *GetStaticObjectField;
    void *GetStaticBooleanField;
    void *GetStaticByteField;
    void *GetStaticCharField;
    void *GetStaticShortField;
    void *GetStaticIntField;
    void *GetStaticLongField;
    void *GetStaticFloatField;
    void *GetStaticDoubleField;
    void *SetStaticObjectField;
    void *SetStaticBooleanField;
    void *SetStaticByteField;
    void *SetStaticCharField;
    void *SetStaticShortField;
    void *SetStaticIntField;
    void *SetStaticLongField;
    void *SetStaticFloatField;
    void *SetStaticDoubleField;
    void *NewString;
    void *GetStringLength;
    void *GetStringChars;
    void *ReleaseStringChars;
    void *NewStringUTF;
    void *GetStringUTFLength;
    void *GetStringUTFChars;
    void *ReleaseStringUTFChars;
    void *GetArrayLength;
    void *NewObjectArray;
    void *GetObjectArrayElement;
    void *SetObjectArrayElement;
    void *NewBooleanArray;
    void *NewByteArray;
    void *NewCharArray;
    void *NewShortArray;
    void *NewIntArray;
    void *NewLongArray;
    void *NewFloatArray;
    void *NewDoubleArray;
    void *GetBooleanArrayElements;
    void *GetByteArrayElements;
    void *GetCharArrayElements;
    void *GetShortArrayElements;
    void *GetIntArrayElements;
    void *GetLongArrayElements;
    void *GetFloatArrayElements;
    void *GetDoubleArrayElements;
    void *ReleaseBooleanArrayElements;
    void *ReleaseByteArrayElements;
    void *ReleaseCharArrayElements;
    void *ReleaseShortArrayElements;
    void *ReleaseIntArrayElements;
    void *ReleaseLongArrayElements;
    void *ReleaseFloatArrayElements;
    void *ReleaseDoubleArrayElements;
    void *GetBooleanArrayRegion;
    void *GetByteArrayRegion;
    void *GetCharArrayRegion;
    void *GetShortArrayRegion;
    void *GetIntArrayRegion;
    void *GetLongArrayRegion;
    void *GetFloatArrayRegion;
    void *GetDoubleArrayRegion;
    void *SetBooleanArrayRegion;
    void *SetByteArrayRegion;
    void *SetCharArrayRegion;
    void *SetShortArrayRegion;
    void *SetIntArrayRegion;
    void *SetLongArrayRegion;
    void *SetFloatArrayRegion;
    void *SetDoubleArrayRegion;
    void *RegisterNatives;
    void *UnregisterNatives;
    void *MonitorEnter;
    void *MonitorExit;
    void *GetJavaVM;
    void *GetStringRegion;
    void *GetStringUTFRegion;
    void *GetPrimitiveArrayCritical;
    void *ReleasePrimitiveArrayCritical;
    void *GetStringCritical;
    void *ReleaseStringCritical;
    void *NewWeakGlobalRef;
    void *DeleteWeakGlobalRef;
    void *ExceptionCheck;
    void *NewDirectByteBuffer;
    void *GetDirectBufferAddress;
    void *GetDirectBufferCapacity;
    void *GetObjectRefType;
};

struct JNIInvokeInterface_ {
    void *reserved0, *reserved1, *reserved2;
    void *DestroyJavaVM;
    void *AttachCurrentThread;
    void *DetachCurrentThread;
    void *GetEnv;
    void *AttachCurrentThreadAsDaemon;
};

/* The single process-wide env and vm. JNIEnv is a pointer to the function
 * table, and a JNIEnv* handed to the engine is the address of that pointer. */
static struct JNINativeInterface_ g_env_table;
static const struct JNINativeInterface_ *g_env = &g_env_table;
static struct JNIInvokeInterface_ g_vm_table;
static const struct JNIInvokeInterface_ *g_jvm = &g_vm_table;

/* --- string interning: FindClass/GetMethodID/... hand back stable, non-NULL
 * opaque IDs that are just interned C strings, so handles are comparable and
 * self-describing in the log. --- */
#define JNI_INTERN_MAX 8192
static char *g_intern[JNI_INTERN_MAX];
static int g_intern_n;
static CRITICAL_SECTION g_jni_lock;
static int g_jni_lock_ready;

static void *jni_intern(const char *s)
{
    void *r = NULL;
    int i;
    if (!s)
        s = "?";
    EnterCriticalSection(&g_jni_lock);
    for (i = 0; i < g_intern_n; i++)
        if (strcmp(g_intern[i], s) == 0) { r = g_intern[i]; break; }
    if (!r && g_intern_n < JNI_INTERN_MAX) {
        g_intern[g_intern_n] = _strdup(s);
        r = g_intern[g_intern_n];
        g_intern_n++;
    }
    LeaveCriticalSection(&g_jni_lock);
    return r ? r : (void *)"?";
}

/* Every unimplemented slot funnels through here so a run prints exactly what the
 * engine asks of the Java side. Returns NULL (good for the pointer/int slots). */
static void *jni_trace(const char *fn)
{
    qx_log("[jni] %s", fn);
    return NULL;
}

/* --- real (minimal) implementations for the startup path --- */
static SYSV jint jr_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }

static SYSV void *jr_FindClass(void *env, const char *name)
{
    (void)env;
    qx_log("[jni] FindClass %s", name ? name : "?");
    return jni_intern(name);
}
static SYSV void *jr_GetMethodID(void *env, void *cls, const char *name, const char *sig)
{
    char b[512];
    (void)env;
    snprintf(b, sizeof b, "%s.%s%s", cls ? (char *)cls : "?", name ? name : "?", sig ? sig : "");
    qx_log("[jni] GetMethodID %s", b);
    return jni_intern(b);
}
static SYSV void *jr_GetStaticMethodID(void *env, void *cls, const char *name, const char *sig)
{
    char b[512];
    (void)env;
    snprintf(b, sizeof b, "static %s.%s%s", cls ? (char *)cls : "?", name ? name : "?", sig ? sig : "");
    qx_log("[jni] GetStaticMethodID %s", b);
    return jni_intern(b);
}
static SYSV void *jr_GetFieldID(void *env, void *cls, const char *name, const char *sig)
{
    char b[512];
    (void)env;
    snprintf(b, sizeof b, "%s.%s:%s", cls ? (char *)cls : "?", name ? name : "?", sig ? sig : "?");
    qx_log("[jni] GetFieldID %s", b);
    return jni_intern(b);
}
static SYSV void *jr_GetStaticFieldID(void *env, void *cls, const char *name, const char *sig)
{
    char b[512];
    (void)env;
    snprintf(b, sizeof b, "static %s.%s:%s", cls ? (char *)cls : "?", name ? name : "?", sig ? sig : "?");
    qx_log("[jni] GetStaticFieldID %s", b);
    return jni_intern(b);
}
static SYSV jint jr_RegisterNatives(void *env, void *cls, const void *methods, jint n)
{
    (void)env; (void)methods;
    qx_log("[jni] RegisterNatives %s (%d method(s))", cls ? (char *)cls : "?", (int)n);
    return JNI_OK;
}
static SYSV jint jr_UnregisterNatives(void *env, void *cls) { (void)env; (void)cls; return JNI_OK; }
static SYSV jboolean jr_ExceptionCheck(void *env) { (void)env; return JNI_FALSE; }
static SYSV void *jr_ExceptionOccurred(void *env) { (void)env; return NULL; }
static SYSV void jr_ExceptionClear(void *env) { (void)env; }
static SYSV void *jr_ref_identity(void *env, void *o) { (void)env; return o; }
static SYSV void jr_ref_delete(void *env, void *o) { (void)env; (void)o; }
static SYSV jboolean jr_IsSameObject(void *env, void *a, void *b) { (void)env; return a == b; }
static SYSV jint jr_GetJavaVM(void *env, void **vm) { (void)env; if (vm) *vm = (void *)&g_jvm; return JNI_OK; }
static SYSV void *jr_GetObjectClass(void *env, void *o) { (void)env; (void)o; return jni_intern("<obj-class>"); }
static SYSV void *jr_NewStringUTF(void *env, const char *b) { (void)env; return b ? (void *)_strdup(b) : NULL; }
static SYSV const char *jr_GetStringUTFChars(void *env, void *s, jboolean *iscopy)
{
    (void)env;
    if (iscopy) *iscopy = JNI_FALSE;
    return (const char *)s;
}
static SYSV void jr_ReleaseStringUTFChars(void *env, void *s, const char *c) { (void)env; (void)s; (void)c; }
static SYSV jsize jr_GetStringUTFLength(void *env, void *s) { (void)env; return s ? (jsize)strlen((char *)s) : 0; }
static SYSV jint jr_EnsureLocalCapacity(void *env, jint n) { (void)env; (void)n; return JNI_OK; }
static SYSV jint jr_PushLocalFrame(void *env, jint n) { (void)env; (void)n; return JNI_OK; }
static SYSV void *jr_PopLocalFrame(void *env, void *r) { (void)env; return r; }
static SYSV void jr_FatalError(void *env, const char *m) { (void)env; qx_err("[jni] FatalError: %s", m ? m : "?"); ExitProcess(134); }

/* JavaVM (invoke interface) */
static SYSV jint jr_GetEnv(void *vm, void **penv, jint ver) { (void)vm; (void)ver; if (penv) *penv = (void *)&g_env; return JNI_OK; }
static SYSV jint jr_AttachCurrentThread(void *vm, void **penv, void *args) { (void)vm; (void)args; if (penv) *penv = (void *)&g_env; return JNI_OK; }
static SYSV jint jr_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static SYSV jint jr_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }

static SYSV void *js_GetVersion(void){ return jni_trace("GetVersion"); }
static SYSV void *js_DefineClass(void){ return jni_trace("DefineClass"); }
static SYSV void *js_FindClass(void){ return jni_trace("FindClass"); }
static SYSV void *js_FromReflectedMethod(void){ return jni_trace("FromReflectedMethod"); }
static SYSV void *js_FromReflectedField(void){ return jni_trace("FromReflectedField"); }
static SYSV void *js_ToReflectedMethod(void){ return jni_trace("ToReflectedMethod"); }
static SYSV void *js_GetSuperclass(void){ return jni_trace("GetSuperclass"); }
static SYSV void *js_IsAssignableFrom(void){ return jni_trace("IsAssignableFrom"); }
static SYSV void *js_ToReflectedField(void){ return jni_trace("ToReflectedField"); }
static SYSV void *js_Throw(void){ return jni_trace("Throw"); }
static SYSV void *js_ThrowNew(void){ return jni_trace("ThrowNew"); }
static SYSV void *js_ExceptionOccurred(void){ return jni_trace("ExceptionOccurred"); }
static SYSV void *js_ExceptionDescribe(void){ return jni_trace("ExceptionDescribe"); }
static SYSV void *js_ExceptionClear(void){ return jni_trace("ExceptionClear"); }
static SYSV void *js_FatalError(void){ return jni_trace("FatalError"); }
static SYSV void *js_PushLocalFrame(void){ return jni_trace("PushLocalFrame"); }
static SYSV void *js_PopLocalFrame(void){ return jni_trace("PopLocalFrame"); }
static SYSV void *js_NewGlobalRef(void){ return jni_trace("NewGlobalRef"); }
static SYSV void *js_DeleteGlobalRef(void){ return jni_trace("DeleteGlobalRef"); }
static SYSV void *js_DeleteLocalRef(void){ return jni_trace("DeleteLocalRef"); }
static SYSV void *js_IsSameObject(void){ return jni_trace("IsSameObject"); }
static SYSV void *js_NewLocalRef(void){ return jni_trace("NewLocalRef"); }
static SYSV void *js_EnsureLocalCapacity(void){ return jni_trace("EnsureLocalCapacity"); }
static SYSV void *js_AllocObject(void){ return jni_trace("AllocObject"); }
static SYSV void *js_NewObject(void){ return jni_trace("NewObject"); }
static SYSV void *js_NewObjectV(void){ return jni_trace("NewObjectV"); }
static SYSV void *js_NewObjectA(void){ return jni_trace("NewObjectA"); }
static SYSV void *js_GetObjectClass(void){ return jni_trace("GetObjectClass"); }
static SYSV void *js_IsInstanceOf(void){ return jni_trace("IsInstanceOf"); }
static SYSV void *js_GetMethodID(void){ return jni_trace("GetMethodID"); }
static SYSV void *js_CallObjectMethod(void){ return jni_trace("CallObjectMethod"); }
static SYSV void *js_CallObjectMethodV(void){ return jni_trace("CallObjectMethodV"); }
static SYSV void *js_CallObjectMethodA(void){ return jni_trace("CallObjectMethodA"); }
static SYSV void *js_CallBooleanMethod(void){ return jni_trace("CallBooleanMethod"); }
static SYSV void *js_CallBooleanMethodV(void){ return jni_trace("CallBooleanMethodV"); }
static SYSV void *js_CallBooleanMethodA(void){ return jni_trace("CallBooleanMethodA"); }
static SYSV void *js_CallByteMethod(void){ return jni_trace("CallByteMethod"); }
static SYSV void *js_CallByteMethodV(void){ return jni_trace("CallByteMethodV"); }
static SYSV void *js_CallByteMethodA(void){ return jni_trace("CallByteMethodA"); }
static SYSV void *js_CallCharMethod(void){ return jni_trace("CallCharMethod"); }
static SYSV void *js_CallCharMethodV(void){ return jni_trace("CallCharMethodV"); }
static SYSV void *js_CallCharMethodA(void){ return jni_trace("CallCharMethodA"); }
static SYSV void *js_CallShortMethod(void){ return jni_trace("CallShortMethod"); }
static SYSV void *js_CallShortMethodV(void){ return jni_trace("CallShortMethodV"); }
static SYSV void *js_CallShortMethodA(void){ return jni_trace("CallShortMethodA"); }
static SYSV void *js_CallIntMethod(void){ return jni_trace("CallIntMethod"); }
static SYSV void *js_CallIntMethodV(void){ return jni_trace("CallIntMethodV"); }
static SYSV void *js_CallIntMethodA(void){ return jni_trace("CallIntMethodA"); }
static SYSV void *js_CallLongMethod(void){ return jni_trace("CallLongMethod"); }
static SYSV void *js_CallLongMethodV(void){ return jni_trace("CallLongMethodV"); }
static SYSV void *js_CallLongMethodA(void){ return jni_trace("CallLongMethodA"); }
static SYSV double js_CallFloatMethod(void){ jni_trace("CallFloatMethod"); return 0; }
static SYSV double js_CallFloatMethodV(void){ jni_trace("CallFloatMethodV"); return 0; }
static SYSV double js_CallFloatMethodA(void){ jni_trace("CallFloatMethodA"); return 0; }
static SYSV double js_CallDoubleMethod(void){ jni_trace("CallDoubleMethod"); return 0; }
static SYSV double js_CallDoubleMethodV(void){ jni_trace("CallDoubleMethodV"); return 0; }
static SYSV double js_CallDoubleMethodA(void){ jni_trace("CallDoubleMethodA"); return 0; }
static SYSV void *js_CallVoidMethod(void){ return jni_trace("CallVoidMethod"); }
static SYSV void *js_CallVoidMethodV(void){ return jni_trace("CallVoidMethodV"); }
static SYSV void *js_CallVoidMethodA(void){ return jni_trace("CallVoidMethodA"); }
static SYSV void *js_CallNonvirtualObjectMethod(void){ return jni_trace("CallNonvirtualObjectMethod"); }
static SYSV void *js_CallNonvirtualObjectMethodV(void){ return jni_trace("CallNonvirtualObjectMethodV"); }
static SYSV void *js_CallNonvirtualObjectMethodA(void){ return jni_trace("CallNonvirtualObjectMethodA"); }
static SYSV void *js_CallNonvirtualBooleanMethod(void){ return jni_trace("CallNonvirtualBooleanMethod"); }
static SYSV void *js_CallNonvirtualBooleanMethodV(void){ return jni_trace("CallNonvirtualBooleanMethodV"); }
static SYSV void *js_CallNonvirtualBooleanMethodA(void){ return jni_trace("CallNonvirtualBooleanMethodA"); }
static SYSV void *js_CallNonvirtualByteMethod(void){ return jni_trace("CallNonvirtualByteMethod"); }
static SYSV void *js_CallNonvirtualByteMethodV(void){ return jni_trace("CallNonvirtualByteMethodV"); }
static SYSV void *js_CallNonvirtualByteMethodA(void){ return jni_trace("CallNonvirtualByteMethodA"); }
static SYSV void *js_CallNonvirtualCharMethod(void){ return jni_trace("CallNonvirtualCharMethod"); }
static SYSV void *js_CallNonvirtualCharMethodV(void){ return jni_trace("CallNonvirtualCharMethodV"); }
static SYSV void *js_CallNonvirtualCharMethodA(void){ return jni_trace("CallNonvirtualCharMethodA"); }
static SYSV void *js_CallNonvirtualShortMethod(void){ return jni_trace("CallNonvirtualShortMethod"); }
static SYSV void *js_CallNonvirtualShortMethodV(void){ return jni_trace("CallNonvirtualShortMethodV"); }
static SYSV void *js_CallNonvirtualShortMethodA(void){ return jni_trace("CallNonvirtualShortMethodA"); }
static SYSV void *js_CallNonvirtualIntMethod(void){ return jni_trace("CallNonvirtualIntMethod"); }
static SYSV void *js_CallNonvirtualIntMethodV(void){ return jni_trace("CallNonvirtualIntMethodV"); }
static SYSV void *js_CallNonvirtualIntMethodA(void){ return jni_trace("CallNonvirtualIntMethodA"); }
static SYSV void *js_CallNonvirtualLongMethod(void){ return jni_trace("CallNonvirtualLongMethod"); }
static SYSV void *js_CallNonvirtualLongMethodV(void){ return jni_trace("CallNonvirtualLongMethodV"); }
static SYSV void *js_CallNonvirtualLongMethodA(void){ return jni_trace("CallNonvirtualLongMethodA"); }
static SYSV double js_CallNonvirtualFloatMethod(void){ jni_trace("CallNonvirtualFloatMethod"); return 0; }
static SYSV double js_CallNonvirtualFloatMethodV(void){ jni_trace("CallNonvirtualFloatMethodV"); return 0; }
static SYSV double js_CallNonvirtualFloatMethodA(void){ jni_trace("CallNonvirtualFloatMethodA"); return 0; }
static SYSV double js_CallNonvirtualDoubleMethod(void){ jni_trace("CallNonvirtualDoubleMethod"); return 0; }
static SYSV double js_CallNonvirtualDoubleMethodV(void){ jni_trace("CallNonvirtualDoubleMethodV"); return 0; }
static SYSV double js_CallNonvirtualDoubleMethodA(void){ jni_trace("CallNonvirtualDoubleMethodA"); return 0; }
static SYSV void *js_CallNonvirtualVoidMethod(void){ return jni_trace("CallNonvirtualVoidMethod"); }
static SYSV void *js_CallNonvirtualVoidMethodV(void){ return jni_trace("CallNonvirtualVoidMethodV"); }
static SYSV void *js_CallNonvirtualVoidMethodA(void){ return jni_trace("CallNonvirtualVoidMethodA"); }
static SYSV void *js_GetFieldID(void){ return jni_trace("GetFieldID"); }
static SYSV void *js_GetObjectField(void){ return jni_trace("GetObjectField"); }
static SYSV void *js_GetBooleanField(void){ return jni_trace("GetBooleanField"); }
static SYSV void *js_GetByteField(void){ return jni_trace("GetByteField"); }
static SYSV void *js_GetCharField(void){ return jni_trace("GetCharField"); }
static SYSV void *js_GetShortField(void){ return jni_trace("GetShortField"); }
static SYSV void *js_GetIntField(void){ return jni_trace("GetIntField"); }
static SYSV void *js_GetLongField(void){ return jni_trace("GetLongField"); }
static SYSV double js_GetFloatField(void){ jni_trace("GetFloatField"); return 0; }
static SYSV double js_GetDoubleField(void){ jni_trace("GetDoubleField"); return 0; }
static SYSV void *js_SetObjectField(void){ return jni_trace("SetObjectField"); }
static SYSV void *js_SetBooleanField(void){ return jni_trace("SetBooleanField"); }
static SYSV void *js_SetByteField(void){ return jni_trace("SetByteField"); }
static SYSV void *js_SetCharField(void){ return jni_trace("SetCharField"); }
static SYSV void *js_SetShortField(void){ return jni_trace("SetShortField"); }
static SYSV void *js_SetIntField(void){ return jni_trace("SetIntField"); }
static SYSV void *js_SetLongField(void){ return jni_trace("SetLongField"); }
static SYSV void *js_SetFloatField(void){ return jni_trace("SetFloatField"); }
static SYSV void *js_SetDoubleField(void){ return jni_trace("SetDoubleField"); }
static SYSV void *js_GetStaticMethodID(void){ return jni_trace("GetStaticMethodID"); }
static SYSV void *js_CallStaticObjectMethod(void){ return jni_trace("CallStaticObjectMethod"); }
static SYSV void *js_CallStaticObjectMethodV(void){ return jni_trace("CallStaticObjectMethodV"); }
static SYSV void *js_CallStaticObjectMethodA(void){ return jni_trace("CallStaticObjectMethodA"); }
static SYSV void *js_CallStaticBooleanMethod(void){ return jni_trace("CallStaticBooleanMethod"); }
static SYSV void *js_CallStaticBooleanMethodV(void){ return jni_trace("CallStaticBooleanMethodV"); }
static SYSV void *js_CallStaticBooleanMethodA(void){ return jni_trace("CallStaticBooleanMethodA"); }
static SYSV void *js_CallStaticByteMethod(void){ return jni_trace("CallStaticByteMethod"); }
static SYSV void *js_CallStaticByteMethodV(void){ return jni_trace("CallStaticByteMethodV"); }
static SYSV void *js_CallStaticByteMethodA(void){ return jni_trace("CallStaticByteMethodA"); }
static SYSV void *js_CallStaticCharMethod(void){ return jni_trace("CallStaticCharMethod"); }
static SYSV void *js_CallStaticCharMethodV(void){ return jni_trace("CallStaticCharMethodV"); }
static SYSV void *js_CallStaticCharMethodA(void){ return jni_trace("CallStaticCharMethodA"); }
static SYSV void *js_CallStaticShortMethod(void){ return jni_trace("CallStaticShortMethod"); }
static SYSV void *js_CallStaticShortMethodV(void){ return jni_trace("CallStaticShortMethodV"); }
static SYSV void *js_CallStaticShortMethodA(void){ return jni_trace("CallStaticShortMethodA"); }
static SYSV void *js_CallStaticIntMethod(void){ return jni_trace("CallStaticIntMethod"); }
static SYSV void *js_CallStaticIntMethodV(void){ return jni_trace("CallStaticIntMethodV"); }
static SYSV void *js_CallStaticIntMethodA(void){ return jni_trace("CallStaticIntMethodA"); }
static SYSV void *js_CallStaticLongMethod(void){ return jni_trace("CallStaticLongMethod"); }
static SYSV void *js_CallStaticLongMethodV(void){ return jni_trace("CallStaticLongMethodV"); }
static SYSV void *js_CallStaticLongMethodA(void){ return jni_trace("CallStaticLongMethodA"); }
static SYSV double js_CallStaticFloatMethod(void){ jni_trace("CallStaticFloatMethod"); return 0; }
static SYSV double js_CallStaticFloatMethodV(void){ jni_trace("CallStaticFloatMethodV"); return 0; }
static SYSV double js_CallStaticFloatMethodA(void){ jni_trace("CallStaticFloatMethodA"); return 0; }
static SYSV double js_CallStaticDoubleMethod(void){ jni_trace("CallStaticDoubleMethod"); return 0; }
static SYSV double js_CallStaticDoubleMethodV(void){ jni_trace("CallStaticDoubleMethodV"); return 0; }
static SYSV double js_CallStaticDoubleMethodA(void){ jni_trace("CallStaticDoubleMethodA"); return 0; }
static SYSV void *js_CallStaticVoidMethod(void){ return jni_trace("CallStaticVoidMethod"); }
static SYSV void *js_CallStaticVoidMethodV(void){ return jni_trace("CallStaticVoidMethodV"); }
static SYSV void *js_CallStaticVoidMethodA(void){ return jni_trace("CallStaticVoidMethodA"); }
static SYSV void *js_GetStaticFieldID(void){ return jni_trace("GetStaticFieldID"); }
static SYSV void *js_GetStaticObjectField(void){ return jni_trace("GetStaticObjectField"); }
static SYSV void *js_GetStaticBooleanField(void){ return jni_trace("GetStaticBooleanField"); }
static SYSV void *js_GetStaticByteField(void){ return jni_trace("GetStaticByteField"); }
static SYSV void *js_GetStaticCharField(void){ return jni_trace("GetStaticCharField"); }
static SYSV void *js_GetStaticShortField(void){ return jni_trace("GetStaticShortField"); }
static SYSV void *js_GetStaticIntField(void){ return jni_trace("GetStaticIntField"); }
static SYSV void *js_GetStaticLongField(void){ return jni_trace("GetStaticLongField"); }
static SYSV double js_GetStaticFloatField(void){ jni_trace("GetStaticFloatField"); return 0; }
static SYSV double js_GetStaticDoubleField(void){ jni_trace("GetStaticDoubleField"); return 0; }
static SYSV void *js_SetStaticObjectField(void){ return jni_trace("SetStaticObjectField"); }
static SYSV void *js_SetStaticBooleanField(void){ return jni_trace("SetStaticBooleanField"); }
static SYSV void *js_SetStaticByteField(void){ return jni_trace("SetStaticByteField"); }
static SYSV void *js_SetStaticCharField(void){ return jni_trace("SetStaticCharField"); }
static SYSV void *js_SetStaticShortField(void){ return jni_trace("SetStaticShortField"); }
static SYSV void *js_SetStaticIntField(void){ return jni_trace("SetStaticIntField"); }
static SYSV void *js_SetStaticLongField(void){ return jni_trace("SetStaticLongField"); }
static SYSV void *js_SetStaticFloatField(void){ return jni_trace("SetStaticFloatField"); }
static SYSV void *js_SetStaticDoubleField(void){ return jni_trace("SetStaticDoubleField"); }
static SYSV void *js_NewString(void){ return jni_trace("NewString"); }
static SYSV void *js_GetStringLength(void){ return jni_trace("GetStringLength"); }
static SYSV void *js_GetStringChars(void){ return jni_trace("GetStringChars"); }
static SYSV void *js_ReleaseStringChars(void){ return jni_trace("ReleaseStringChars"); }
static SYSV void *js_NewStringUTF(void){ return jni_trace("NewStringUTF"); }
static SYSV void *js_GetStringUTFLength(void){ return jni_trace("GetStringUTFLength"); }
static SYSV void *js_GetStringUTFChars(void){ return jni_trace("GetStringUTFChars"); }
static SYSV void *js_ReleaseStringUTFChars(void){ return jni_trace("ReleaseStringUTFChars"); }
static SYSV void *js_GetArrayLength(void){ return jni_trace("GetArrayLength"); }
static SYSV void *js_NewObjectArray(void){ return jni_trace("NewObjectArray"); }
static SYSV void *js_GetObjectArrayElement(void){ return jni_trace("GetObjectArrayElement"); }
static SYSV void *js_SetObjectArrayElement(void){ return jni_trace("SetObjectArrayElement"); }
static SYSV void *js_NewBooleanArray(void){ return jni_trace("NewBooleanArray"); }
static SYSV void *js_NewByteArray(void){ return jni_trace("NewByteArray"); }
static SYSV void *js_NewCharArray(void){ return jni_trace("NewCharArray"); }
static SYSV void *js_NewShortArray(void){ return jni_trace("NewShortArray"); }
static SYSV void *js_NewIntArray(void){ return jni_trace("NewIntArray"); }
static SYSV void *js_NewLongArray(void){ return jni_trace("NewLongArray"); }
static SYSV void *js_NewFloatArray(void){ return jni_trace("NewFloatArray"); }
static SYSV void *js_NewDoubleArray(void){ return jni_trace("NewDoubleArray"); }
static SYSV void *js_GetBooleanArrayElements(void){ return jni_trace("GetBooleanArrayElements"); }
static SYSV void *js_GetByteArrayElements(void){ return jni_trace("GetByteArrayElements"); }
static SYSV void *js_GetCharArrayElements(void){ return jni_trace("GetCharArrayElements"); }
static SYSV void *js_GetShortArrayElements(void){ return jni_trace("GetShortArrayElements"); }
static SYSV void *js_GetIntArrayElements(void){ return jni_trace("GetIntArrayElements"); }
static SYSV void *js_GetLongArrayElements(void){ return jni_trace("GetLongArrayElements"); }
static SYSV void *js_GetFloatArrayElements(void){ return jni_trace("GetFloatArrayElements"); }
static SYSV void *js_GetDoubleArrayElements(void){ return jni_trace("GetDoubleArrayElements"); }
static SYSV void *js_ReleaseBooleanArrayElements(void){ return jni_trace("ReleaseBooleanArrayElements"); }
static SYSV void *js_ReleaseByteArrayElements(void){ return jni_trace("ReleaseByteArrayElements"); }
static SYSV void *js_ReleaseCharArrayElements(void){ return jni_trace("ReleaseCharArrayElements"); }
static SYSV void *js_ReleaseShortArrayElements(void){ return jni_trace("ReleaseShortArrayElements"); }
static SYSV void *js_ReleaseIntArrayElements(void){ return jni_trace("ReleaseIntArrayElements"); }
static SYSV void *js_ReleaseLongArrayElements(void){ return jni_trace("ReleaseLongArrayElements"); }
static SYSV void *js_ReleaseFloatArrayElements(void){ return jni_trace("ReleaseFloatArrayElements"); }
static SYSV void *js_ReleaseDoubleArrayElements(void){ return jni_trace("ReleaseDoubleArrayElements"); }
static SYSV void *js_GetBooleanArrayRegion(void){ return jni_trace("GetBooleanArrayRegion"); }
static SYSV void *js_GetByteArrayRegion(void){ return jni_trace("GetByteArrayRegion"); }
static SYSV void *js_GetCharArrayRegion(void){ return jni_trace("GetCharArrayRegion"); }
static SYSV void *js_GetShortArrayRegion(void){ return jni_trace("GetShortArrayRegion"); }
static SYSV void *js_GetIntArrayRegion(void){ return jni_trace("GetIntArrayRegion"); }
static SYSV void *js_GetLongArrayRegion(void){ return jni_trace("GetLongArrayRegion"); }
static SYSV void *js_GetFloatArrayRegion(void){ return jni_trace("GetFloatArrayRegion"); }
static SYSV void *js_GetDoubleArrayRegion(void){ return jni_trace("GetDoubleArrayRegion"); }
static SYSV void *js_SetBooleanArrayRegion(void){ return jni_trace("SetBooleanArrayRegion"); }
static SYSV void *js_SetByteArrayRegion(void){ return jni_trace("SetByteArrayRegion"); }
static SYSV void *js_SetCharArrayRegion(void){ return jni_trace("SetCharArrayRegion"); }
static SYSV void *js_SetShortArrayRegion(void){ return jni_trace("SetShortArrayRegion"); }
static SYSV void *js_SetIntArrayRegion(void){ return jni_trace("SetIntArrayRegion"); }
static SYSV void *js_SetLongArrayRegion(void){ return jni_trace("SetLongArrayRegion"); }
static SYSV void *js_SetFloatArrayRegion(void){ return jni_trace("SetFloatArrayRegion"); }
static SYSV void *js_SetDoubleArrayRegion(void){ return jni_trace("SetDoubleArrayRegion"); }
static SYSV void *js_RegisterNatives(void){ return jni_trace("RegisterNatives"); }
static SYSV void *js_UnregisterNatives(void){ return jni_trace("UnregisterNatives"); }
static SYSV void *js_MonitorEnter(void){ return jni_trace("MonitorEnter"); }
static SYSV void *js_MonitorExit(void){ return jni_trace("MonitorExit"); }
static SYSV void *js_GetJavaVM(void){ return jni_trace("GetJavaVM"); }
static SYSV void *js_GetStringRegion(void){ return jni_trace("GetStringRegion"); }
static SYSV void *js_GetStringUTFRegion(void){ return jni_trace("GetStringUTFRegion"); }
static SYSV void *js_GetPrimitiveArrayCritical(void){ return jni_trace("GetPrimitiveArrayCritical"); }
static SYSV void *js_ReleasePrimitiveArrayCritical(void){ return jni_trace("ReleasePrimitiveArrayCritical"); }
static SYSV void *js_GetStringCritical(void){ return jni_trace("GetStringCritical"); }
static SYSV void *js_ReleaseStringCritical(void){ return jni_trace("ReleaseStringCritical"); }
static SYSV void *js_NewWeakGlobalRef(void){ return jni_trace("NewWeakGlobalRef"); }
static SYSV void *js_DeleteWeakGlobalRef(void){ return jni_trace("DeleteWeakGlobalRef"); }
static SYSV void *js_ExceptionCheck(void){ return jni_trace("ExceptionCheck"); }
static SYSV void *js_NewDirectByteBuffer(void){ return jni_trace("NewDirectByteBuffer"); }
static SYSV void *js_GetDirectBufferAddress(void){ return jni_trace("GetDirectBufferAddress"); }
static SYSV void *js_GetDirectBufferCapacity(void){ return jni_trace("GetDirectBufferCapacity"); }
static SYSV void *js_GetObjectRefType(void){ return jni_trace("GetObjectRefType"); }

/* Fill every slot with its logging stub; the real ones are set afterwards. */
static void jni_fill(struct JNINativeInterface_ *t)
{
    t->GetVersion = (void *)js_GetVersion;
    t->DefineClass = (void *)js_DefineClass;
    t->FindClass = (void *)js_FindClass;
    t->FromReflectedMethod = (void *)js_FromReflectedMethod;
    t->FromReflectedField = (void *)js_FromReflectedField;
    t->ToReflectedMethod = (void *)js_ToReflectedMethod;
    t->GetSuperclass = (void *)js_GetSuperclass;
    t->IsAssignableFrom = (void *)js_IsAssignableFrom;
    t->ToReflectedField = (void *)js_ToReflectedField;
    t->Throw = (void *)js_Throw;
    t->ThrowNew = (void *)js_ThrowNew;
    t->ExceptionOccurred = (void *)js_ExceptionOccurred;
    t->ExceptionDescribe = (void *)js_ExceptionDescribe;
    t->ExceptionClear = (void *)js_ExceptionClear;
    t->FatalError = (void *)js_FatalError;
    t->PushLocalFrame = (void *)js_PushLocalFrame;
    t->PopLocalFrame = (void *)js_PopLocalFrame;
    t->NewGlobalRef = (void *)js_NewGlobalRef;
    t->DeleteGlobalRef = (void *)js_DeleteGlobalRef;
    t->DeleteLocalRef = (void *)js_DeleteLocalRef;
    t->IsSameObject = (void *)js_IsSameObject;
    t->NewLocalRef = (void *)js_NewLocalRef;
    t->EnsureLocalCapacity = (void *)js_EnsureLocalCapacity;
    t->AllocObject = (void *)js_AllocObject;
    t->NewObject = (void *)js_NewObject;
    t->NewObjectV = (void *)js_NewObjectV;
    t->NewObjectA = (void *)js_NewObjectA;
    t->GetObjectClass = (void *)js_GetObjectClass;
    t->IsInstanceOf = (void *)js_IsInstanceOf;
    t->GetMethodID = (void *)js_GetMethodID;
    t->CallObjectMethod = (void *)js_CallObjectMethod;
    t->CallObjectMethodV = (void *)js_CallObjectMethodV;
    t->CallObjectMethodA = (void *)js_CallObjectMethodA;
    t->CallBooleanMethod = (void *)js_CallBooleanMethod;
    t->CallBooleanMethodV = (void *)js_CallBooleanMethodV;
    t->CallBooleanMethodA = (void *)js_CallBooleanMethodA;
    t->CallByteMethod = (void *)js_CallByteMethod;
    t->CallByteMethodV = (void *)js_CallByteMethodV;
    t->CallByteMethodA = (void *)js_CallByteMethodA;
    t->CallCharMethod = (void *)js_CallCharMethod;
    t->CallCharMethodV = (void *)js_CallCharMethodV;
    t->CallCharMethodA = (void *)js_CallCharMethodA;
    t->CallShortMethod = (void *)js_CallShortMethod;
    t->CallShortMethodV = (void *)js_CallShortMethodV;
    t->CallShortMethodA = (void *)js_CallShortMethodA;
    t->CallIntMethod = (void *)js_CallIntMethod;
    t->CallIntMethodV = (void *)js_CallIntMethodV;
    t->CallIntMethodA = (void *)js_CallIntMethodA;
    t->CallLongMethod = (void *)js_CallLongMethod;
    t->CallLongMethodV = (void *)js_CallLongMethodV;
    t->CallLongMethodA = (void *)js_CallLongMethodA;
    t->CallFloatMethod = (void *)js_CallFloatMethod;
    t->CallFloatMethodV = (void *)js_CallFloatMethodV;
    t->CallFloatMethodA = (void *)js_CallFloatMethodA;
    t->CallDoubleMethod = (void *)js_CallDoubleMethod;
    t->CallDoubleMethodV = (void *)js_CallDoubleMethodV;
    t->CallDoubleMethodA = (void *)js_CallDoubleMethodA;
    t->CallVoidMethod = (void *)js_CallVoidMethod;
    t->CallVoidMethodV = (void *)js_CallVoidMethodV;
    t->CallVoidMethodA = (void *)js_CallVoidMethodA;
    t->CallNonvirtualObjectMethod = (void *)js_CallNonvirtualObjectMethod;
    t->CallNonvirtualObjectMethodV = (void *)js_CallNonvirtualObjectMethodV;
    t->CallNonvirtualObjectMethodA = (void *)js_CallNonvirtualObjectMethodA;
    t->CallNonvirtualBooleanMethod = (void *)js_CallNonvirtualBooleanMethod;
    t->CallNonvirtualBooleanMethodV = (void *)js_CallNonvirtualBooleanMethodV;
    t->CallNonvirtualBooleanMethodA = (void *)js_CallNonvirtualBooleanMethodA;
    t->CallNonvirtualByteMethod = (void *)js_CallNonvirtualByteMethod;
    t->CallNonvirtualByteMethodV = (void *)js_CallNonvirtualByteMethodV;
    t->CallNonvirtualByteMethodA = (void *)js_CallNonvirtualByteMethodA;
    t->CallNonvirtualCharMethod = (void *)js_CallNonvirtualCharMethod;
    t->CallNonvirtualCharMethodV = (void *)js_CallNonvirtualCharMethodV;
    t->CallNonvirtualCharMethodA = (void *)js_CallNonvirtualCharMethodA;
    t->CallNonvirtualShortMethod = (void *)js_CallNonvirtualShortMethod;
    t->CallNonvirtualShortMethodV = (void *)js_CallNonvirtualShortMethodV;
    t->CallNonvirtualShortMethodA = (void *)js_CallNonvirtualShortMethodA;
    t->CallNonvirtualIntMethod = (void *)js_CallNonvirtualIntMethod;
    t->CallNonvirtualIntMethodV = (void *)js_CallNonvirtualIntMethodV;
    t->CallNonvirtualIntMethodA = (void *)js_CallNonvirtualIntMethodA;
    t->CallNonvirtualLongMethod = (void *)js_CallNonvirtualLongMethod;
    t->CallNonvirtualLongMethodV = (void *)js_CallNonvirtualLongMethodV;
    t->CallNonvirtualLongMethodA = (void *)js_CallNonvirtualLongMethodA;
    t->CallNonvirtualFloatMethod = (void *)js_CallNonvirtualFloatMethod;
    t->CallNonvirtualFloatMethodV = (void *)js_CallNonvirtualFloatMethodV;
    t->CallNonvirtualFloatMethodA = (void *)js_CallNonvirtualFloatMethodA;
    t->CallNonvirtualDoubleMethod = (void *)js_CallNonvirtualDoubleMethod;
    t->CallNonvirtualDoubleMethodV = (void *)js_CallNonvirtualDoubleMethodV;
    t->CallNonvirtualDoubleMethodA = (void *)js_CallNonvirtualDoubleMethodA;
    t->CallNonvirtualVoidMethod = (void *)js_CallNonvirtualVoidMethod;
    t->CallNonvirtualVoidMethodV = (void *)js_CallNonvirtualVoidMethodV;
    t->CallNonvirtualVoidMethodA = (void *)js_CallNonvirtualVoidMethodA;
    t->GetFieldID = (void *)js_GetFieldID;
    t->GetObjectField = (void *)js_GetObjectField;
    t->GetBooleanField = (void *)js_GetBooleanField;
    t->GetByteField = (void *)js_GetByteField;
    t->GetCharField = (void *)js_GetCharField;
    t->GetShortField = (void *)js_GetShortField;
    t->GetIntField = (void *)js_GetIntField;
    t->GetLongField = (void *)js_GetLongField;
    t->GetFloatField = (void *)js_GetFloatField;
    t->GetDoubleField = (void *)js_GetDoubleField;
    t->SetObjectField = (void *)js_SetObjectField;
    t->SetBooleanField = (void *)js_SetBooleanField;
    t->SetByteField = (void *)js_SetByteField;
    t->SetCharField = (void *)js_SetCharField;
    t->SetShortField = (void *)js_SetShortField;
    t->SetIntField = (void *)js_SetIntField;
    t->SetLongField = (void *)js_SetLongField;
    t->SetFloatField = (void *)js_SetFloatField;
    t->SetDoubleField = (void *)js_SetDoubleField;
    t->GetStaticMethodID = (void *)js_GetStaticMethodID;
    t->CallStaticObjectMethod = (void *)js_CallStaticObjectMethod;
    t->CallStaticObjectMethodV = (void *)js_CallStaticObjectMethodV;
    t->CallStaticObjectMethodA = (void *)js_CallStaticObjectMethodA;
    t->CallStaticBooleanMethod = (void *)js_CallStaticBooleanMethod;
    t->CallStaticBooleanMethodV = (void *)js_CallStaticBooleanMethodV;
    t->CallStaticBooleanMethodA = (void *)js_CallStaticBooleanMethodA;
    t->CallStaticByteMethod = (void *)js_CallStaticByteMethod;
    t->CallStaticByteMethodV = (void *)js_CallStaticByteMethodV;
    t->CallStaticByteMethodA = (void *)js_CallStaticByteMethodA;
    t->CallStaticCharMethod = (void *)js_CallStaticCharMethod;
    t->CallStaticCharMethodV = (void *)js_CallStaticCharMethodV;
    t->CallStaticCharMethodA = (void *)js_CallStaticCharMethodA;
    t->CallStaticShortMethod = (void *)js_CallStaticShortMethod;
    t->CallStaticShortMethodV = (void *)js_CallStaticShortMethodV;
    t->CallStaticShortMethodA = (void *)js_CallStaticShortMethodA;
    t->CallStaticIntMethod = (void *)js_CallStaticIntMethod;
    t->CallStaticIntMethodV = (void *)js_CallStaticIntMethodV;
    t->CallStaticIntMethodA = (void *)js_CallStaticIntMethodA;
    t->CallStaticLongMethod = (void *)js_CallStaticLongMethod;
    t->CallStaticLongMethodV = (void *)js_CallStaticLongMethodV;
    t->CallStaticLongMethodA = (void *)js_CallStaticLongMethodA;
    t->CallStaticFloatMethod = (void *)js_CallStaticFloatMethod;
    t->CallStaticFloatMethodV = (void *)js_CallStaticFloatMethodV;
    t->CallStaticFloatMethodA = (void *)js_CallStaticFloatMethodA;
    t->CallStaticDoubleMethod = (void *)js_CallStaticDoubleMethod;
    t->CallStaticDoubleMethodV = (void *)js_CallStaticDoubleMethodV;
    t->CallStaticDoubleMethodA = (void *)js_CallStaticDoubleMethodA;
    t->CallStaticVoidMethod = (void *)js_CallStaticVoidMethod;
    t->CallStaticVoidMethodV = (void *)js_CallStaticVoidMethodV;
    t->CallStaticVoidMethodA = (void *)js_CallStaticVoidMethodA;
    t->GetStaticFieldID = (void *)js_GetStaticFieldID;
    t->GetStaticObjectField = (void *)js_GetStaticObjectField;
    t->GetStaticBooleanField = (void *)js_GetStaticBooleanField;
    t->GetStaticByteField = (void *)js_GetStaticByteField;
    t->GetStaticCharField = (void *)js_GetStaticCharField;
    t->GetStaticShortField = (void *)js_GetStaticShortField;
    t->GetStaticIntField = (void *)js_GetStaticIntField;
    t->GetStaticLongField = (void *)js_GetStaticLongField;
    t->GetStaticFloatField = (void *)js_GetStaticFloatField;
    t->GetStaticDoubleField = (void *)js_GetStaticDoubleField;
    t->SetStaticObjectField = (void *)js_SetStaticObjectField;
    t->SetStaticBooleanField = (void *)js_SetStaticBooleanField;
    t->SetStaticByteField = (void *)js_SetStaticByteField;
    t->SetStaticCharField = (void *)js_SetStaticCharField;
    t->SetStaticShortField = (void *)js_SetStaticShortField;
    t->SetStaticIntField = (void *)js_SetStaticIntField;
    t->SetStaticLongField = (void *)js_SetStaticLongField;
    t->SetStaticFloatField = (void *)js_SetStaticFloatField;
    t->SetStaticDoubleField = (void *)js_SetStaticDoubleField;
    t->NewString = (void *)js_NewString;
    t->GetStringLength = (void *)js_GetStringLength;
    t->GetStringChars = (void *)js_GetStringChars;
    t->ReleaseStringChars = (void *)js_ReleaseStringChars;
    t->NewStringUTF = (void *)js_NewStringUTF;
    t->GetStringUTFLength = (void *)js_GetStringUTFLength;
    t->GetStringUTFChars = (void *)js_GetStringUTFChars;
    t->ReleaseStringUTFChars = (void *)js_ReleaseStringUTFChars;
    t->GetArrayLength = (void *)js_GetArrayLength;
    t->NewObjectArray = (void *)js_NewObjectArray;
    t->GetObjectArrayElement = (void *)js_GetObjectArrayElement;
    t->SetObjectArrayElement = (void *)js_SetObjectArrayElement;
    t->NewBooleanArray = (void *)js_NewBooleanArray;
    t->NewByteArray = (void *)js_NewByteArray;
    t->NewCharArray = (void *)js_NewCharArray;
    t->NewShortArray = (void *)js_NewShortArray;
    t->NewIntArray = (void *)js_NewIntArray;
    t->NewLongArray = (void *)js_NewLongArray;
    t->NewFloatArray = (void *)js_NewFloatArray;
    t->NewDoubleArray = (void *)js_NewDoubleArray;
    t->GetBooleanArrayElements = (void *)js_GetBooleanArrayElements;
    t->GetByteArrayElements = (void *)js_GetByteArrayElements;
    t->GetCharArrayElements = (void *)js_GetCharArrayElements;
    t->GetShortArrayElements = (void *)js_GetShortArrayElements;
    t->GetIntArrayElements = (void *)js_GetIntArrayElements;
    t->GetLongArrayElements = (void *)js_GetLongArrayElements;
    t->GetFloatArrayElements = (void *)js_GetFloatArrayElements;
    t->GetDoubleArrayElements = (void *)js_GetDoubleArrayElements;
    t->ReleaseBooleanArrayElements = (void *)js_ReleaseBooleanArrayElements;
    t->ReleaseByteArrayElements = (void *)js_ReleaseByteArrayElements;
    t->ReleaseCharArrayElements = (void *)js_ReleaseCharArrayElements;
    t->ReleaseShortArrayElements = (void *)js_ReleaseShortArrayElements;
    t->ReleaseIntArrayElements = (void *)js_ReleaseIntArrayElements;
    t->ReleaseLongArrayElements = (void *)js_ReleaseLongArrayElements;
    t->ReleaseFloatArrayElements = (void *)js_ReleaseFloatArrayElements;
    t->ReleaseDoubleArrayElements = (void *)js_ReleaseDoubleArrayElements;
    t->GetBooleanArrayRegion = (void *)js_GetBooleanArrayRegion;
    t->GetByteArrayRegion = (void *)js_GetByteArrayRegion;
    t->GetCharArrayRegion = (void *)js_GetCharArrayRegion;
    t->GetShortArrayRegion = (void *)js_GetShortArrayRegion;
    t->GetIntArrayRegion = (void *)js_GetIntArrayRegion;
    t->GetLongArrayRegion = (void *)js_GetLongArrayRegion;
    t->GetFloatArrayRegion = (void *)js_GetFloatArrayRegion;
    t->GetDoubleArrayRegion = (void *)js_GetDoubleArrayRegion;
    t->SetBooleanArrayRegion = (void *)js_SetBooleanArrayRegion;
    t->SetByteArrayRegion = (void *)js_SetByteArrayRegion;
    t->SetCharArrayRegion = (void *)js_SetCharArrayRegion;
    t->SetShortArrayRegion = (void *)js_SetShortArrayRegion;
    t->SetIntArrayRegion = (void *)js_SetIntArrayRegion;
    t->SetLongArrayRegion = (void *)js_SetLongArrayRegion;
    t->SetFloatArrayRegion = (void *)js_SetFloatArrayRegion;
    t->SetDoubleArrayRegion = (void *)js_SetDoubleArrayRegion;
    t->RegisterNatives = (void *)js_RegisterNatives;
    t->UnregisterNatives = (void *)js_UnregisterNatives;
    t->MonitorEnter = (void *)js_MonitorEnter;
    t->MonitorExit = (void *)js_MonitorExit;
    t->GetJavaVM = (void *)js_GetJavaVM;
    t->GetStringRegion = (void *)js_GetStringRegion;
    t->GetStringUTFRegion = (void *)js_GetStringUTFRegion;
    t->GetPrimitiveArrayCritical = (void *)js_GetPrimitiveArrayCritical;
    t->ReleasePrimitiveArrayCritical = (void *)js_ReleasePrimitiveArrayCritical;
    t->GetStringCritical = (void *)js_GetStringCritical;
    t->ReleaseStringCritical = (void *)js_ReleaseStringCritical;
    t->NewWeakGlobalRef = (void *)js_NewWeakGlobalRef;
    t->DeleteWeakGlobalRef = (void *)js_DeleteWeakGlobalRef;
    t->ExceptionCheck = (void *)js_ExceptionCheck;
    t->NewDirectByteBuffer = (void *)js_NewDirectByteBuffer;
    t->GetDirectBufferAddress = (void *)js_GetDirectBufferAddress;
    t->GetDirectBufferCapacity = (void *)js_GetDirectBufferCapacity;
    t->GetObjectRefType = (void *)js_GetObjectRefType;

    /* real implementations for the startup path */
    t->GetVersion = (void *)jr_GetVersion;
    t->FindClass = (void *)jr_FindClass;
    t->GetMethodID = (void *)jr_GetMethodID;
    t->GetStaticMethodID = (void *)jr_GetStaticMethodID;
    t->GetFieldID = (void *)jr_GetFieldID;
    t->GetStaticFieldID = (void *)jr_GetStaticFieldID;
    t->RegisterNatives = (void *)jr_RegisterNatives;
    t->UnregisterNatives = (void *)jr_UnregisterNatives;
    t->ExceptionCheck = (void *)jr_ExceptionCheck;
    t->ExceptionOccurred = (void *)jr_ExceptionOccurred;
    t->ExceptionClear = (void *)jr_ExceptionClear;
    t->NewGlobalRef = (void *)jr_ref_identity;
    t->NewLocalRef = (void *)jr_ref_identity;
    t->NewWeakGlobalRef = (void *)jr_ref_identity;
    t->DeleteGlobalRef = (void *)jr_ref_delete;
    t->DeleteLocalRef = (void *)jr_ref_delete;
    t->DeleteWeakGlobalRef = (void *)jr_ref_delete;
    t->IsSameObject = (void *)jr_IsSameObject;
    t->GetJavaVM = (void *)jr_GetJavaVM;
    t->GetObjectClass = (void *)jr_GetObjectClass;
    t->NewStringUTF = (void *)jr_NewStringUTF;
    t->GetStringUTFChars = (void *)jr_GetStringUTFChars;
    t->ReleaseStringUTFChars = (void *)jr_ReleaseStringUTFChars;
    t->GetStringUTFLength = (void *)jr_GetStringUTFLength;
    t->EnsureLocalCapacity = (void *)jr_EnsureLocalCapacity;
    t->PushLocalFrame = (void *)jr_PushLocalFrame;
    t->PopLocalFrame = (void *)jr_PopLocalFrame;
    t->FatalError = (void *)jr_FatalError;
}

static int g_jni_ready;
static void jni_init(void)
{
    if (g_jni_ready)
        return;
    if (!g_jni_lock_ready) { InitializeCriticalSection(&g_jni_lock); g_jni_lock_ready = 1; }
    jni_fill(&g_env_table);
    g_vm_table.DestroyJavaVM = (void *)jr_DestroyJavaVM;
    g_vm_table.AttachCurrentThread = (void *)jr_AttachCurrentThread;
    g_vm_table.DetachCurrentThread = (void *)jr_DetachCurrentThread;
    g_vm_table.GetEnv = (void *)jr_GetEnv;
    g_vm_table.AttachCurrentThreadAsDaemon = (void *)jr_AttachCurrentThread;
    g_jni_ready = 1;
}

/* JavaVM* / JNIEnv* for callers that drive the engine's JNI entry points. */
void *qx_jni_javavm(void) { jni_init(); return (void *)&g_jvm; }
void *qx_jni_env(void) { jni_init(); return (void *)&g_env; }

/* Call the engine's JNI_OnLoad with our synthetic VM. Returns its version, or
 * -1 if the pointer is NULL. */
int qx_jni_call_onload(void *jni_onload)
{
    typedef jint(SYSV * onload_t)(void *, void *);
    jint v;
    jni_init();
    if (!jni_onload)
        return -1;
    qx_log("[jni] calling JNI_OnLoad");
    v = ((onload_t)jni_onload)((void *)&g_jvm, NULL);
    qx_log("[jni] JNI_OnLoad returned 0x%x", (unsigned)v);
    return (int)v;
}
