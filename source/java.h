#ifndef JAVA_H
#define JAVA_H

#include <falso_jni/FalsoJNI.h>

extern jobject java_thiz;

typedef jboolean (*native_bool_fn)(JNIEnv *, jobject);
typedef void (*native_void_fn)(JNIEnv *, jobject);
typedef void (*native_str_fn)(JNIEnv *, jobject, jstring);

extern native_str_fn NativeCallback_KeyboardFinished;

#endif
