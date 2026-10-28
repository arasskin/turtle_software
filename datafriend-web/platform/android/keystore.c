#ifdef __ANDROID__
#include <jni.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
static JavaVM *vm;
static jclass store_class;
static jobject app_context;
static jmethodID load_method;
static pthread_mutex_t setup_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool failed(JNIEnv *env) {
  if (!(*env)->ExceptionCheck(env))
    return false;
  /* Never format provider exceptions: their messages may contain secrets. */
  (*env)->ExceptionClear(env);
  return true;
}
JNIEXPORT void JNICALL Java_dev_drawdo_datafriend_IdentityStore_initialize(JNIEnv *env, jclass cls,
                                                                           jobject context) {
  pthread_mutex_lock(&setup_mutex);
  if (app_context)
    goto done;
  if ((*env)->GetJavaVM(env, &vm) != JNI_OK)
    goto done;
  jclass type = (*env)->NewGlobalRef(env, cls);
  jobject app = NULL;
  jmethodID method = NULL;
  if (!(*env)->ExceptionCheck(env))
    app = (*env)->NewGlobalRef(env, context);
  if (!(*env)->ExceptionCheck(env))
    method = (*env)->GetStaticMethodID(env, cls, "loadOrCreate",
                                       "(Landroid/content/Context;Ljava/lang/String;[B)[B");
  if (!type || !app || !method || (*env)->ExceptionCheck(env)) {
    if (type)
      (*env)->DeleteGlobalRef(env, type);
    if (app)
      (*env)->DeleteGlobalRef(env, app);
    goto done; /* Deliver pending exception to Java install(). */
  }
  store_class = type;
  app_context = app;
  load_method = method;
done:
  pthread_mutex_unlock(&setup_mutex);
}
const char *df_identity_load(const char *alias, uint8_t seed[32]) {
  const char *error = "Datafriend Android secure identity storage failed";
  JNIEnv *env = NULL;
  bool attached = false, frame = false;
  pthread_mutex_lock(&setup_mutex);
  if (!app_context) {
    error = "Datafriend Android identity storage is not initialized";
    goto done;
  }
  jint rc = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
  if (rc == JNI_EDETACHED) {
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK)
      goto done;
    attached = true;
  } else if (rc != JNI_OK)
    goto done;
  if (!env)
    goto done;
  if ((*env)->PushLocalFrame(env, 8) != JNI_OK) {
    failed(env);
    goto done;
  }
  frame = true;
  jstring name = (*env)->NewStringUTF(env, alias);
  if (failed(env) || !name)
    goto done;
  jbyteArray candidate = (*env)->NewByteArray(env, 32);
  if (failed(env) || !candidate)
    goto done;
  (*env)->SetByteArrayRegion(env, candidate, 0, 32, (const jbyte *)seed);
  if (failed(env))
    goto done;
  jbyteArray loaded = (jbyteArray)(*env)->CallStaticObjectMethod(env, store_class, load_method,
                                                                 app_context, name, candidate);
  /* Java wipes candidate in finally, including storage/provider failures. */
  if (failed(env) || !loaded)
    goto done;
  jsize length = (*env)->GetArrayLength(env, loaded);
  if (failed(env) || length != 32)
    goto done;
  (*env)->GetByteArrayRegion(env, loaded, 0, 32, (jbyte *)seed);
  bool copy_failed = failed(env);
  jbyte zero[32] = {0};
  (*env)->SetByteArrayRegion(env, loaded, 0, 32, zero);
  if (failed(env) || copy_failed)
    goto done;
  error = NULL;
done:
  if (frame)
    (*env)->PopLocalFrame(env, NULL);
  if (attached)
    (*vm)->DetachCurrentThread(vm);
  pthread_mutex_unlock(&setup_mutex);
  return error;
}
#endif
