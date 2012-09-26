/*
 * This file is part of libbluray
 * Copyright (C) 2010  William Hahne
 * Copyright (C) 2012  Petri Hintukainen <phintuka@users.sourceforge.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.s
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "bdj.h"

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include "bdj_private.h"
#include "bdjo_parser.h"
#include "bdj_util.h"
#include "common.h"
#include "libbluray/register.h"
#include "file/dl.h"
#include "util/strutl.h"
#include "util/macro.h"
#include "libbluray/bdnav/bdid_parse.h"

#include <jni.h>
#include <stdlib.h>
#include <string.h>

typedef jint (JNICALL * fptr_JNI_CreateJavaVM) (JavaVM **pvm, void **penv,void *args);

static void *_load_jvm(void)
{
    const char* java_home = getenv("JAVA_HOME"); // FIXME: should probably search multiple directories
    if (java_home == NULL) {
        BD_DEBUG(DBG_BDJ | DBG_CRIT, "JAVA_HOME not set, can't find Java VM.\n");
        return NULL;
    }

#ifdef WIN32
    char* path = str_printf("%s/jre/bin/server/jvm", java_home);
#else	//	#ifdef WIN32
    char* path = str_printf("%s/jre/lib/%s/server/libjvm", java_home, JAVA_ARCH);
#endif	//	#ifdef WIN32

    return dl_dlopen(path, NULL);
}

static int _bdj_init(BDJAVA *bdjava, JNIEnv *env)
{
    // initialize class org.videolan.Libbluray
    jclass init_class;
    jmethodID init_id;
    if (!bdj_get_method(env, &init_class, &init_id,
                        "org/videolan/Libbluray", "init", "(JLjava/lang/String;)V")) {
        return 0;
    }

    char* id_path = str_printf("%s/CERTIFICATE/id.bdmv", bdjava->path);
    BDID_DATA *id  = bdid_parse(id_path);
    jlong param_bdjava_ptr = (jlong)(intptr_t) bdjava;
    jstring param_disc_id = (*env)->NewStringUTF(env,
                                                 id ? id->disc_id : "00000000000000000000000000000000");
    (*env)->CallStaticVoidMethod(env, init_class, init_id,
                                 param_bdjava_ptr, param_disc_id);
    (*env)->DeleteLocalRef(env, init_class);
    (*env)->DeleteLocalRef(env, param_disc_id);

    free(id_path);
    bdid_free(&id);

    return 1;
}

BDJAVA* bdj_open(const char *path,
                 struct bluray *bd, struct bd_registers_s *registers,
                 struct indx_root_s *index)
{
    // first load the jvm using dlopen
    void* jvm_lib = _load_jvm();

    if (!jvm_lib) {
        BD_DEBUG(DBG_BDJ | DBG_CRIT, "Wasn't able to load libjvm.so\n");
        return NULL;
    }

    fptr_JNI_CreateJavaVM JNI_CreateJavaVM_fp = (fptr_JNI_CreateJavaVM)dl_dlsym(jvm_lib, "JNI_CreateJavaVM");

    if (JNI_CreateJavaVM_fp == NULL) {
        BD_DEBUG(DBG_BDJ | DBG_CRIT, "Couldn't find symbol JNI_CreateJavaVM.\n");
        return NULL;
    }

    BDJAVA* bdjava = malloc(sizeof(BDJAVA));
    bdjava->bd = bd;
    bdjava->reg = registers;
    bdjava->index = index;
    bdjava->path = path;

    JavaVMInitArgs args;

    // check if overriding the classpath
    const char* classpath = getenv("LIBBLURAY_CP");
    if (classpath == NULL)
        classpath = BDJ_CLASSPATH;

    // determine classpath
    char* classpath_opt = str_printf("-Djava.class.path=%s", classpath);

    // determine bluray.vfs.root
    char* vfs_opt;
    vfs_opt = str_printf("-Dbluray.vfs.root=%s", path);

    JavaVMOption* option = malloc(sizeof(JavaVMOption) * 9);
    int n = 0;
    option[n++].optionString = classpath_opt;
    option[n++].optionString = vfs_opt;

    args.version = JNI_VERSION_1_6;
    args.nOptions = n;
    args.options = option;
    args.ignoreUnrecognized = JNI_FALSE; // don't ignore unrecognized options

    int result = JNI_CreateJavaVM_fp(&bdjava->jvm, (void**) &bdjava->env, &args);
    free(option);
    free(classpath_opt);
    free(vfs_opt);

    if (result != JNI_OK || !bdjava->env) {
        free(bdjava);
        BD_DEBUG(DBG_BDJ | DBG_CRIT, "Failed to create new Java VM.\n");
        return NULL;
    }

    _bdj_init(bdjava, bdjava->env);

    return bdjava;
}

int bdj_start(BDJAVA *bdjava, unsigned title)
{
    JNIEnv* env = bdjava->env;
    jboolean status = JNI_FALSE;
    jclass loader_class;
    jmethodID load_id;

    if (bdj_get_method(env, &loader_class, &load_id,
                       "org/videolan/BDJLoader", "load", "(I)Z")) {
        status = (*env)->CallStaticBooleanMethod(env, loader_class, load_id, (jint)title);
    }

    return (status == JNI_TRUE) ? BDJ_SUCCESS : BDJ_ERROR;
}

int bdj_stop(BDJAVA *bdjava)
{
    JNIEnv* env = bdjava->env;
    jboolean status = JNI_FALSE;
    jclass loader_class;
    jmethodID unload_id;

    if (!bdjava) {
        return BDJ_ERROR;
    }

    if (bdj_get_method(env, &loader_class, &unload_id,
                       "org/videolan/BDJLoader", "unload", "()Z")) {
        status = (*env)->CallStaticBooleanMethod(env, loader_class, unload_id);
    }

    return (status == JNI_TRUE) ? BDJ_SUCCESS : BDJ_ERROR;
}

void bdj_close(BDJAVA *bdjava)
{
    JNIEnv* env = bdjava->env;
    jclass shutdown_class;
    jmethodID shutdown_id;

    if (!bdjava) {
        return;
    }

    if (bdj_get_method(env, &shutdown_class, &shutdown_id,
                       "org/videolan/Libbluray", "shutdown", "()V")) {
        (*env)->CallStaticVoidMethod(env, shutdown_class, shutdown_id);
    }

    (*bdjava->jvm)->DestroyJavaVM(bdjava->jvm);

    free(bdjava);
}

void bdj_process_event(BDJAVA *bdjava, unsigned ev, unsigned param)
{
    JNIEnv* env = bdjava->env;
    jclass event_class;
    jmethodID event_id;

    if (bdj_get_method(env, &event_class, &event_id,
                       "org/videolan/Libbluray", "processEvent", "(II)V")) {
        (*env)->CallStaticVoidMethod(env, event_class, event_id, ev, param);
    }
}
