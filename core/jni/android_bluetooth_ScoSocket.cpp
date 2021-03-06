/*
** Copyright 2008, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "bluetooth_ScoSocket.cpp"

#include "android_bluetooth_common.h"
#include "android_runtime/AndroidRuntime.h"
#include "JNIHelp.h"
#include "jni.h"
#include "utils/Log.h"
#include "utils/misc.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/poll.h>

#ifdef HAVE_BLUETOOTH
#include <bluetooth/bluetooth.h>
#include <bluetooth/sco.h>
#endif

/* Ideally, blocking I/O on a SCO socket would return when another thread
 * calls close(). However it does not right now, in fact close() on a SCO
 * socket has strange behavior (returns a bogus value) when other threads
 * are performing blocking I/O on that socket. So, to workaround, we always
 * call close() from the same thread that does blocking I/O. This requires the
 * use of a socketpair to signal the blocking I/O to abort.
 *
 * Unfortunately I don't know a way to abort connect() yet, but at least this
 * times out after the BT page timeout (10 seconds currently), so the thread
 * will die eventually. The fact that the thread can outlive
 * the Java object forces us to use a mutex in destoryNative().
 *
 * The JNI API is entirely async.
 *
 * Also note this class deals only with SCO connections, not with data
 * transmission.
 */
namespace android {
#ifdef HAVE_BLUETOOTH

static JavaVM *jvm;
static jfieldID field_mNativeData;
static jmethodID method_onAccepted;
static jmethodID method_onConnected;
static jmethodID method_onClosed;

struct thread_data_t;
static void *work_thread(void *arg);
static int connect_work(const char *address);
static int accept_work(int signal_sk);
static void wait_for_close(int sk, int signal_sk);
static void closeNative(JNIEnv *env, jobject object);

/* shared native data - protected by mutex */
typedef struct {
    pthread_mutex_t mutex;
    int signal_sk;        // socket to signal blocked I/O to unblock
    jobject object;       // JNI global ref to the Java object
    thread_data_t *thread_data;  // pointer to thread local data
                                 // max 1 thread per sco socket
} native_data_t;

/* thread local data */
struct thread_data_t {
    native_data_t *nat;
    bool is_accept;        // accept (listening) or connect (outgoing) thread
    int signal_sk;         // socket for thread to listen for unblock signal
    char address[BTADDR_SIZE];  // BT addres as string
};

static inline native_data_t * get_native_data(JNIEnv *env, jobject object) {
    return (native_data_t *)(env->GetIntField(object, field_mNativeData));
}
#endif

static void classInitNative(JNIEnv* env, jclass clazz) {
    LOGV(__FUNCTION__);
#ifdef HAVE_BLUETOOTH
    if (env->GetJavaVM(&jvm) < 0) {
        LOGE("Could not get handle to the VM");
    }
    field_mNativeData = get_field(env, clazz, "mNativeData", "I");
    method_onAccepted = env->GetMethodID(clazz, "onAccepted", "(I)V");
    method_onConnected = env->GetMethodID(clazz, "onConnected", "(I)V");
    method_onClosed = env->GetMethodID(clazz, "onClosed", "()V");
#endif
}

/* Returns false if a serious error occured */
static jboolean initNative(JNIEnv* env, jobject object) {
    LOGV(__FUNCTION__);
#ifdef HAVE_BLUETOOTH

    native_data_t *nat = (native_data_t *) calloc(1, sizeof(native_data_t));
    if (nat == NULL) {
        LOGE("%s: out of memory!", __FUNCTION__);
        return JNI_FALSE;
    }

    pthread_mutex_init(&nat->mutex, NULL);
    env->SetIntField(object, field_mNativeData, (jint)nat);
    nat->signal_sk = -1;
    nat->object = NULL;
    nat->thread_data = NULL;

#endif
    return JNI_TRUE;
}

static void destroyNative(JNIEnv* env, jobject object) {
    LOGV(__FUNCTION__);
#ifdef HAVE_BLUETOOTH
    native_data_t *nat = get_native_data(env, object);
    
    closeNative(env, object);
    
    pthread_mutex_lock(&nat->mutex);
    if (nat->thread_data != NULL) {
        nat->thread_data->nat = NULL;
    }
    pthread_mutex_unlock(&nat->mutex);
    pthread_mutex_destroy(&nat->mutex);

    free(nat);
#endif
}

static jboolean acceptNative(JNIEnv *env, jobject object) {
    LOGV(__FUNCTION__);
#ifdef HAVE_BLUETOOTH
    native_data_t *nat = get_native_data(env, object);
    int signal_sks[2];
    pthread_t thread;
    struct thread_data_t *data = NULL;

    pthread_mutex_lock(&nat->mutex);
    if (nat->signal_sk != -1) {
        pthread_mutex_unlock(&nat->mutex);
        return JNI_FALSE;
    }

    // setup socketpair to pass messages between threads
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, signal_sks) < 0) {
        LOGE("%s: socketpair() failed: %s", __FUNCTION__, strerror(errno));
        pthread_mutex_unlock(&nat->mutex);
        return JNI_FALSE;
    }
    nat->signal_sk = signal_sks[0];
    nat->object = env->NewGlobalRef(object);

    data = (thread_data_t *)calloc(1, sizeof(thread_data_t));
    if (data == NULL) {
        LOGE("%s: out of memory", __FUNCTION__);
        pthread_mutex_unlock(&nat->mutex);
        return JNI_FALSE;
    }
    nat->thread_data = data;
    pthread_mutex_unlock(&nat->mutex);

    data->signal_sk = signal_sks[1];
    data->nat = nat;
    data->is_accept = true;

    if (pthread_create(&thread, NULL, &work_thread, (void *)data) < 0) {
        LOGE("%s: pthread_create() failed: %s", __FUNCTION__, strerror(errno));
        return JNI_FALSE;
    }
    return JNI_TRUE;

#endif
    return JNI_FALSE;
}

static jboolean connectNative(JNIEnv *env, jobject object, jstring address) {
    LOGV(__FUNCTION__);
#ifdef HAVE_BLUETOOTH
    native_data_t *nat = get_native_data(env, object);
    int signal_sks[2];
    pthread_t thread;
    struct thread_data_t *data;
    const char *c_address;

    pthread_mutex_lock(&nat->mutex);
    if (nat->signal_sk != -1) {
        pthread_mutex_unlock(&nat->mutex);
        return JNI_FALSE;
    }

    // setup socketpair to pass messages between threads
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, signal_sks) < 0) {
        LOGE("%s: socketpair() failed: %s\n", __FUNCTION__, strerror(errno));
        pthread_mutex_unlock(&nat->mutex);
        return JNI_FALSE;
    }
    nat->signal_sk = signal_sks[0];
    nat->object = env->NewGlobalRef(object);

    data = (thread_data_t *)calloc(1, sizeof(thread_data_t));
    if (data == NULL) {
        LOGE("%s: out of memory", __FUNCTION__);
        pthread_mutex_unlock(&nat->mutex);
        return JNI_FALSE;
    }
    pthread_mutex_unlock(&nat->mutex);

    data->signal_sk = signal_sks[1];
    data->nat = nat;
    c_address = env->GetStringUTFChars(address, NULL);
    strlcpy(data->address, c_address, BTADDR_SIZE);
    env->ReleaseStringUTFChars(address, c_address);
    data->is_accept = false;

    if (pthread_create(&thread, NULL, &work_thread, (void *)data) < 0) {
        LOGE("%s: pthread_create() failed: %s", __FUNCTION__, strerror(errno));
        return JNI_FALSE;
    }
    return JNI_TRUE;

#endif
    return JNI_FALSE;
}

static void closeNative(JNIEnv *env, jobject object) {
    LOGV(__FUNCTION__);
#ifdef HAVE_BLUETOOTH
    native_data_t *nat = get_native_data(env, object);
    int signal_sk;

    pthread_mutex_lock(&nat->mutex);
    signal_sk = nat->signal_sk;
    nat->signal_sk = -1;
    env->DeleteGlobalRef(nat->object);
    nat->object = NULL;
    pthread_mutex_unlock(&nat->mutex);

    if (signal_sk >= 0) {
        LOGV("%s: signal_sk = %d", __FUNCTION__, signal_sk);
        unsigned char dummy;
        write(signal_sk, &dummy, sizeof(dummy));
        close(signal_sk);
    }
#endif
}

#ifdef HAVE_BLUETOOTH
/* thread entry point */
static void *work_thread(void *arg) {
    JNIEnv* env;
    thread_data_t *data = (thread_data_t *)arg;
    int sk;

    LOGV(__FUNCTION__);
    if (jvm->AttachCurrentThread(&env, NULL) != JNI_OK) {
        LOGE("%s: AttachCurrentThread() failed", __FUNCTION__);
        return NULL;
    }

    /* connect the SCO socket */
    if (data->is_accept) {
        LOGV("SCO OBJECT %p ACCEPT #####", data->nat->object);
        sk = accept_work(data->signal_sk);
        LOGV("SCO OBJECT %p END ACCEPT *****", data->nat->object);
    } else {
        sk = connect_work(data->address);
    }

    /* callback with connection result */
    if (data->nat == NULL) {
        LOGV("%s: object destroyed!", __FUNCTION__);
        goto done;
    }
    pthread_mutex_lock(&data->nat->mutex);
    if (data->nat->object == NULL) {
        pthread_mutex_unlock(&data->nat->mutex);
        LOGV("%s: callback cancelled", __FUNCTION__);
        goto done;
    }
    if (data->is_accept) {
        env->CallVoidMethod(data->nat->object, method_onAccepted, sk);
    } else {
        env->CallVoidMethod(data->nat->object, method_onConnected, sk);
    }
    pthread_mutex_unlock(&data->nat->mutex);

    if (sk < 0) {
        goto done;
    }

    LOGV("SCO OBJECT %p %d CONNECTED +++ (%s)", data->nat->object, sk,
         data->is_accept ? "in" : "out");

    /* wait for the socket to close */
    LOGV("wait_for_close()...");
    wait_for_close(sk, data->signal_sk);
    LOGV("wait_for_close() returned");

    /* callback with close result */
    if (data->nat == NULL) {
        LOGV("%s: object destroyed!", __FUNCTION__);
        goto done;
    }
    pthread_mutex_lock(&data->nat->mutex);
    if (data->nat->object == NULL) {
        LOGV("%s: callback cancelled", __FUNCTION__);
    } else {
        env->CallVoidMethod(data->nat->object, method_onClosed);
    }
    pthread_mutex_unlock(&data->nat->mutex);

done:
    if (sk >= 0) {
        close(sk);
        LOGV("SCO OBJECT %p %d CLOSED --- (%s)", data->nat->object, sk, data->is_accept ? "in" : "out");
    }
    if (data->signal_sk >= 0) {
        close(data->signal_sk);
    }
    LOGV("SCO socket closed");

    if (data->nat != NULL) {
        pthread_mutex_lock(&data->nat->mutex);
        env->DeleteGlobalRef(data->nat->object);
        data->nat->object = NULL;
        data->nat->thread_data = NULL;
        pthread_mutex_unlock(&data->nat->mutex);
    }

    free(data);
    if (jvm->DetachCurrentThread() != JNI_OK) {
        LOGE("%s: DetachCurrentThread() failed", __FUNCTION__);
    }

    LOGV("work_thread() done");
    return NULL;
}

static int accept_work(int signal_sk) {
    LOGV(__FUNCTION__);
    int sk;
    int nsk;
    int addr_sz;
    int max_fd;
    fd_set fds;
    struct sockaddr_sco addr;

    sk = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_SCO);
    if (sk < 0) {
        LOGE("%s socket() failed: %s", __FUNCTION__, strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sco_family = AF_BLUETOOTH;
    memcpy(&addr.sco_bdaddr, BDADDR_ANY, sizeof(bdaddr_t));
    if (bind(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        LOGE("%s bind() failed: %s", __FUNCTION__, strerror(errno));
        goto error;
    }

    if (listen(sk, 1)) {
        LOGE("%s: listen() failed: %s", __FUNCTION__, strerror(errno));
        goto error;
    }

    memset(&addr, 0, sizeof(addr));
    addr_sz = sizeof(addr);

    FD_ZERO(&fds);
    FD_SET(sk, &fds);
    FD_SET(signal_sk, &fds);

    max_fd = (sk > signal_sk) ? sk : signal_sk;
    LOGI("Listening SCO socket...");
    while (select(max_fd + 1, &fds, NULL, NULL, NULL) < 0) {
        if (errno != EINTR) {
            LOGE("%s: select() failed: %s", __FUNCTION__, strerror(errno));
            goto error;
        }
        LOGV("%s: select() EINTR, retrying", __FUNCTION__);
    }
    LOGV("select() returned");
    if (FD_ISSET(signal_sk, &fds)) {
        // signal to cancel listening
        LOGV("cancelled listening socket, closing");
        goto error;
    }
    if (!FD_ISSET(sk, &fds)) {
        LOGE("error: select() returned >= 0 with no fds set");
        goto error;
    }

    nsk = accept(sk, (struct sockaddr *)&addr, &addr_sz);
    if (nsk < 0) {
        LOGE("%s: accept() failed: %s", __FUNCTION__, strerror(errno));
        goto error;
    }
    LOGI("Connected SCO socket (incoming)");
    close(sk);  // The listening socket

    return nsk;

error:
    close(sk);

    return -1;
}

static int connect_work(const char *address) {
    LOGV(__FUNCTION__);
    struct sockaddr_sco addr;
    int sk = -1;

    sk = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_SCO);
    if (sk < 0) {
        LOGE("%s: socket() failed: %s", __FUNCTION__, strerror(errno));
        return -1;
    }

    /* Bind to local address */
    memset(&addr, 0, sizeof(addr));
    addr.sco_family = AF_BLUETOOTH;
    memcpy(&addr.sco_bdaddr, BDADDR_ANY, sizeof(bdaddr_t));
    if (bind(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        LOGE("%s: bind() failed: %s", __FUNCTION__, strerror(errno));
        goto error;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sco_family = AF_BLUETOOTH;
    get_bdaddr(address, &addr.sco_bdaddr);
    LOGI("Connecting to socket");
    while (connect(sk, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (errno != EINTR) {
            LOGE("%s: connect() failed: %s", __FUNCTION__, strerror(errno));
            goto error;
        }
        LOGV("%s: connect() EINTR, retrying", __FUNCTION__);
    }
    LOGI("SCO socket connected (outgoing)");

    return sk;

error:
    if (sk >= 0) close(sk);
    return -1;
}

static void wait_for_close(int sk, int signal_sk) {
    LOGV(__FUNCTION__);
    pollfd p[2];

    memset(p, 0, 2 * sizeof(pollfd));
    p[0].fd = sk;
    p[1].fd = signal_sk;
    p[1].events = POLLIN | POLLPRI;

    LOGV("poll...");

    while (poll(p, 2, -1) < 0) {  // blocks
        if (errno != EINTR) {
            LOGE("%s: poll() failed: %s", __FUNCTION__, strerror(errno));
            break;
        }
        LOGV("%s: poll() EINTR, retrying", __FUNCTION__);
    }

    LOGV("poll() returned");
}
#endif

static JNINativeMethod sMethods[] = {
    {"classInitNative", "()V", (void*)classInitNative},
    {"initNative", "()V", (void *)initNative},
    {"destroyNative", "()V", (void *)destroyNative},
    {"connectNative", "(Ljava/lang/String;)Z", (void *)connectNative},
    {"acceptNative", "()Z", (void *)acceptNative},
    {"closeNative", "()V", (void *)closeNative},
};

int register_android_bluetooth_ScoSocket(JNIEnv *env) {
    return AndroidRuntime::registerNativeMethods(env,
            "android/bluetooth/ScoSocket", sMethods, NELEM(sMethods));
}

} /* namespace android */
