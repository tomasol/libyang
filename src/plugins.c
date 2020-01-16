/**
 * @file plugins.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief YANG plugin routines implementation
 *
 * Copyright (c) 2015 - 2018 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "common.h"
#include "extensions.h"
#include "user_types.h"
#include "plugin_config.h"
#include "libyang.h"
#include "parser.h"

/* internal structures storing the plugins */
static struct lllyext_plugin_list *ext_plugins = NULL;
static uint16_t ext_plugins_count = 0; /* size of the ext_plugins array */

static struct lllytype_plugin_list *type_plugins = NULL;
static uint16_t type_plugins_count = 0;

static struct llly_set dlhandlers = {0, 0, {NULL}};
static pthread_mutex_t plugins_lock = PTHREAD_MUTEX_INITIALIZER;

static char **loaded_plugins = NULL; /* both ext and type plugin names */
static uint16_t loaded_plugins_count = 0;

/**
 * @brief reference counter for the plugins, it actually counts number of contexts
 */
static uint32_t plugin_refs;

API const char * const *
llly_get_loaded_plugins(void)
{
    FUN_IN;

    return (const char * const *)loaded_plugins;
}

API int
llly_clean_plugins(void)
{
    FUN_IN;

    unsigned int u;
    int ret = EXIT_SUCCESS;

#ifdef STATIC
    /* lock the extension plugins list */
    pthread_mutex_lock(&plugins_lock);

    if(ext_plugins) {
        free(ext_plugins);
        ext_plugins = NULL;
        ext_plugins_count = 0;
    }

    if(type_plugins) {
        free(type_plugins);
        type_plugins = NULL;
        type_plugins_count = 0;
    }

    for (u = 0; u < loaded_plugins_count; ++u) {
        free(loaded_plugins[u]);
    }
    free(loaded_plugins);
    loaded_plugins = NULL;
    loaded_plugins_count = 0;

    /* unlock the global structures */
    pthread_mutex_unlock(&plugins_lock);
    return ret;
#endif /* STATIC */

    /* lock the extension plugins list */
    pthread_mutex_lock(&plugins_lock);

    if (--plugin_refs) {
        /* there is a context that may refer to the plugins, so we cannot remove them */
        ret = EXIT_FAILURE;
        goto cleanup;
    }

    if (!ext_plugins_count && !type_plugins_count) {
        /* no plugin loaded - nothing to do */
        goto cleanup;
    }

    /* clean the lists */
    free(ext_plugins);
    ext_plugins = NULL;
    ext_plugins_count = 0;

    free(type_plugins);
    type_plugins = NULL;
    type_plugins_count = 0;

    for (u = 0; u < loaded_plugins_count; ++u) {
        free(loaded_plugins[u]);
    }
    free(loaded_plugins);
    loaded_plugins = NULL;
    loaded_plugins_count = 0;

    /* close the dl handlers */
    for (u = 0; u < dlhandlers.number; u++) {
        dlclose(dlhandlers.set.g[u]);
    }
    free(dlhandlers.set.g);
    dlhandlers.set.g = NULL;
    dlhandlers.size = 0;
    dlhandlers.number = 0;

cleanup:
    /* unlock the global structures */
    pthread_mutex_unlock(&plugins_lock);

    return ret;
}

static int
lllytype_load_plugin(void *dlhandler, const char *file_name)
{
    struct lllytype_plugin_list *plugin;
    char *str;
    int *version;

#ifdef STATIC
    return 0;
#endif /* STATIC */

    /* get the plugin data */
    plugin = dlsym(dlhandler, file_name);
    str = dlerror();
    if (str) {
        LOGERR(NULL, LLLY_ESYS, "Processing \"%s\" user type plugin failed, missing plugin list object (%s).", file_name, str);
        return 1;
    }
    version = dlsym(dlhandler, "lllytype_api_version");
    if (dlerror() || *version != LLLYTYPE_API_VERSION) {
        LOGWRN(NULL, "Processing \"%s\" user type plugin failed, wrong API version - %d expected, %d found.",
               file_name, LLLYTYPE_API_VERSION, version ? *version : 0);
        return 1;
    }
    return llly_register_types(plugin, file_name);
}

API int
llly_register_types(struct lllytype_plugin_list *plugin, const char *log_name)
{
    FUN_IN;

    struct lllytype_plugin_list *p;
    uint32_t u, v;

    for (u = 0; plugin[u].name; u++) {
        /* check user type implementations for collisions */
        for (v = 0; v < type_plugins_count; v++) {
            if (!strcmp(plugin[u].name, type_plugins[v].name) &&
                    !strcmp(plugin[u].module, type_plugins[v].module) &&
                    (!plugin[u].revision || !type_plugins[v].revision || !strcmp(plugin[u].revision, type_plugins[v].revision))) {
                LOGERR(NULL, LLLY_ESYS, "Processing \"%s\" extension plugin failed,"
                        "implementation collision for extension %s from module %s%s%s.",
                        log_name, plugin[u].name, plugin[u].module, plugin[u].revision ? "@" : "",
                        plugin[u].revision ? plugin[u].revision : "");
                return 1;
            }
        }
    }

    /* add the new plugins, we have number of new plugins as u */
    p = realloc(type_plugins, (type_plugins_count + u) * sizeof *type_plugins);
    if (!p) {
        LOGMEM(NULL);
        return -1;
    }
    type_plugins = p;
    for (; u; u--) {
        memcpy(&type_plugins[type_plugins_count], &plugin[u - 1], sizeof *plugin);
        type_plugins_count++;
    }

    return 0;
}

static int
lllyext_load_plugin(void *dlhandler, const char *file_name)
{
    struct lllyext_plugin_list *plugin;
    char *str;
    int *version;

#ifdef STATIC
    return 0;
#endif /* STATIC */

    /* get the plugin data */
    plugin = dlsym(dlhandler, file_name);
    str = dlerror();
    if (str) {
        LOGERR(NULL, LLLY_ESYS, "Processing \"%s\" extension plugin failed, missing plugin list object (%s).", file_name, str);
        return 1;
    }
    version = dlsym(dlhandler, "lllyext_api_version");
    if (dlerror() || *version != LLLYEXT_API_VERSION) {
        LOGWRN(NULL, "Processing \"%s\" extension plugin failed, wrong API version - %d expected, %d found.",
               file_name, LLLYEXT_API_VERSION, version ? *version : 0);
        return 1;
    }
    return llly_register_exts(plugin, file_name);
}

API int
llly_register_exts(struct lllyext_plugin_list *plugin, const char *log_name)
{
    FUN_IN;

    struct lllyext_plugin_list *p;
    struct lllyext_plugin_complex *pluginc;
    uint32_t u, v;

    for (u = 0; plugin[u].name; u++) {
        /* check extension implementations for collisions */
        for (v = 0; v < ext_plugins_count; v++) {
            if (!strcmp(plugin[u].name, ext_plugins[v].name) &&
                    !strcmp(plugin[u].module, ext_plugins[v].module) &&
                    (!plugin[u].revision || !ext_plugins[v].revision || !strcmp(plugin[u].revision, ext_plugins[v].revision))) {
                LOGERR(NULL, LLLY_ESYS, "Processing \"%s\" extension plugin failed,"
                        "implementation collision for extension %s from module %s%s%s.",
                        log_name, plugin[u].name, plugin[u].module, plugin[u].revision ? "@" : "",
                        plugin[u].revision ? plugin[u].revision : "");
                return 1;
            }
        }

        /* check for valid supported substatements in case of complex extension */
        if (plugin[u].plugin->type == LLLYEXT_COMPLEX && ((struct lllyext_plugin_complex *)plugin[u].plugin)->substmt) {
            pluginc = (struct lllyext_plugin_complex *)plugin[u].plugin;
            for (v = 0; pluginc->substmt[v].stmt; v++) {
                if (pluginc->substmt[v].stmt >= LLLY_STMT_SUBMODULE ||
                        pluginc->substmt[v].stmt == LLLY_STMT_VERSION ||
                        pluginc->substmt[v].stmt == LLLY_STMT_YINELEM) {
                    LOGERR(NULL, LLLY_EINVAL,
                            "Extension plugin \"%s\" (extension %s) allows not supported extension substatement (%s)",
                            log_name, plugin[u].name, llly_stmt_str[pluginc->substmt[v].stmt]);
                    return 1;
                }
                if (pluginc->substmt[v].cardinality > LLLY_STMT_CARD_MAND &&
                        pluginc->substmt[v].stmt >= LLLY_STMT_MODIFIER &&
                        pluginc->substmt[v].stmt <= LLLY_STMT_STATUS) {
                    LOGERR(NULL, LLLY_EINVAL, "Extension plugin \"%s\" (extension %s) allows multiple instances on \"%s\" "
                           "substatement, which is not supported.",
                           log_name, plugin[u].name, llly_stmt_str[pluginc->substmt[v].stmt]);
                    return 1;
                }
            }
        }
    }

    /* add the new plugins, we have number of new plugins as u */
    p = realloc(ext_plugins, (ext_plugins_count + u) * sizeof *ext_plugins);
    if (!p) {
        LOGMEM(NULL);
        return -1;
    }
    ext_plugins = p;
    for (; u; u--) {
        memcpy(&ext_plugins[ext_plugins_count], &plugin[u - 1], sizeof *plugin);
        ext_plugins_count++;
    }

    return 0;
}

/* spends name */
static void
llly_add_loaded_plugin(char *name)
{
    loaded_plugins = llly_realloc(loaded_plugins, (loaded_plugins_count + 2) * sizeof *loaded_plugins);
    LLLY_CHECK_ERR_RETURN(!loaded_plugins, free(name); LOGMEM(NULL), );
    ++loaded_plugins_count;

    loaded_plugins[loaded_plugins_count - 1] = name;
    loaded_plugins[loaded_plugins_count] = NULL;
}

static void
llly_load_plugins_dir(DIR *dir, const char *dir_path, int ext_or_type)
{
    struct dirent *file;
    size_t len;
    char *str, *name;
    void *dlhandler;
    int ret;

#ifdef STATIC
    return;
#endif /* STATIC */

    while ((file = readdir(dir))) {
        /* required format of the filename is *LLLY_PLUGIN_SUFFIX */
        len = strlen(file->d_name);
        if (len < LLLY_PLUGIN_SUFFIX_LEN + 1 ||
                strcmp(&file->d_name[len - LLLY_PLUGIN_SUFFIX_LEN], LLLY_PLUGIN_SUFFIX)) {
            continue;
        }

        /* and construct the filepath */
        if (asprintf(&str, "%s/%s", dir_path, file->d_name) == -1) {
            LOGMEM(NULL);
            return;
        }

        /* load the plugin */
        dlhandler = dlopen(str, RTLD_NOW);
        if (!dlhandler) {
            LOGERR(NULL, LLLY_ESYS, "Loading \"%s\" as a plugin failed (%s).", str, dlerror());
            free(str);
            continue;
        }
        if (llly_set_contains(&dlhandlers, dlhandler) != -1) {
            /* the plugin is already loaded */
            LOGVRB("Plugin \"%s\" already loaded.", str);
            free(str);

            /* keep the refcount of the shared object correct */
            dlclose(dlhandler);
            continue;
        }
        dlerror();    /* Clear any existing error */

        /* store the name without the suffix */
        name = strndup(file->d_name, len - LLLY_PLUGIN_SUFFIX_LEN);
        if (!name) {
            LOGMEM(NULL);
            dlclose(dlhandler);
            free(str);
            return;
        }

        if (ext_or_type) {
            ret = lllyext_load_plugin(dlhandler, name);
        } else {
            ret = lllytype_load_plugin(dlhandler, name);
        }
        if (!ret) {
            LOGVRB("Plugin \"%s\" successfully loaded.", str);
            /* spends name */
            llly_add_loaded_plugin(name);
            /* keep the handler */
            llly_set_add(&dlhandlers, dlhandler, LLLY_SET_OPT_USEASLIST);
        } else {
            free(name);
            dlclose(dlhandler);
        }
        free(str);

        if (ret == -1) {
            /* finish on error */
            break;
        }
    }
}

API void
llly_load_plugins(void)
{
    FUN_IN;

    DIR* dir;
    const char *pluginsdir;

#ifdef STATIC
    /* lock the extension plugins list */
    pthread_mutex_lock(&plugins_lock);

    ext_plugins = static_load_lyext_plugins(&ext_plugins_count);
    type_plugins = static_load_lytype_plugins(&type_plugins_count);

    int u;
    for (u = 0; u < static_loaded_plugins_count; u++) {
        llly_add_loaded_plugin(strdup(static_loaded_plugins[u]));
    }

    /* unlock the global structures */
    pthread_mutex_unlock(&plugins_lock);
    return;
#endif /* STATIC */

    /* lock the extension plugins list */
    pthread_mutex_lock(&plugins_lock);

    /* increase references */
    ++plugin_refs;

    /* try to get the plugins directory from environment variable */
    pluginsdir = getenv("LLLIBYANG_EXTENSIONS_PLUGINS_DIR");
    if (!pluginsdir) {
        pluginsdir = LLLYEXT_PLUGINS_DIR;
    }

    dir = opendir(pluginsdir);
    if (!dir) {
        /* no directory (or no access to it), no extension plugins */
        LOGWRN(NULL, "Failed to open libyang extensions plugins directory \"%s\" (%s).", pluginsdir, strerror(errno));
    } else {
        llly_load_plugins_dir(dir, pluginsdir, 1);
        closedir(dir);
    }

    /* try to get the plugins directory from environment variable */
    pluginsdir = getenv("LIBYANG_USER_TYPES_PLUGINS_DIR");
    if (!pluginsdir) {
        pluginsdir = LLLY_USER_TYPES_PLUGINS_DIR;
    }

    dir = opendir(pluginsdir);
    if (!dir) {
        /* no directory (or no access to it), no extension plugins */
        LOGWRN(NULL, "Failed to open libyang user types plugins directory \"%s\" (%s).", pluginsdir, strerror(errno));
    } else {
        llly_load_plugins_dir(dir, pluginsdir, 0);
        closedir(dir);
    }

    /* unlock the global structures */
    pthread_mutex_unlock(&plugins_lock);
}

struct lllyext_plugin *
ext_get_plugin(const char *name, const char *module, const char *revision)
{
    uint16_t u;

    assert(name);
    assert(module);

    for (u = 0; u < ext_plugins_count; u++) {
        if (!strcmp(name, ext_plugins[u].name) &&
                !strcmp(module, ext_plugins[u].module) &&
                (!ext_plugins[u].revision || !strcmp(revision, ext_plugins[u].revision))) {
            /* we have the match */
            return ext_plugins[u].plugin;
        }
    }

    /* plugin not found */
    return NULL;
}

API int
lllys_ext_instance_presence(struct lllys_ext *def, struct lllys_ext_instance **ext, uint8_t ext_size)
{
    FUN_IN;

    uint8_t index;

    if (!def || (ext_size && !ext)) {
        LOGARG;
        return -1;
    }

    /* search for the extension instance */
    for (index = 0; index < ext_size; index++) {
        if (ext[index]->module->ctx == def->module->ctx) {
            /* from the same context */
            if (ext[index]->def == def) {
                return index;
            }
        } else {
            /* from different contexts */
            if (llly_strequal0(ext[index]->def->name, def->name)
                    && llly_strequal0(lllys_main_module(ext[index]->def->module)->name, lllys_main_module(def->module)->name)) {
                return index;
            }
        }
    }

    /* not found */
    return -1;
}

API void *
lllys_ext_complex_get_substmt(LLLY_STMT stmt, struct lllys_ext_instance_complex *ext, struct lllyext_substmt **info)
{
    FUN_IN;

    int i;

    if (!ext || !ext->def || !ext->def->plugin || ext->def->plugin->type != LLLYEXT_COMPLEX) {
        LOGARG;
        return NULL;
    }

    if (!ext->substmt) {
        /* no substatement defined in the plugin */
        if (info) {
            *info = NULL;
        }
        return NULL;
    }

    /* search the substatements defined by the plugin */
    for (i = 0; ext->substmt[i].stmt; i++) {
        if (stmt == LLLY_STMT_NODE) {
            if (ext->substmt[i].stmt >= LLLY_STMT_ACTION && ext->substmt[i].stmt <= LLLY_STMT_USES) {
                if (info) {
                    *info = &ext->substmt[i];
                }
                break;
            }
        } else if (ext->substmt[i].stmt == stmt) {
            if (info) {
                *info = &ext->substmt[i];
            }
            break;
        }
    }

    if (ext->substmt[i].stmt) {
        return &ext->content[ext->substmt[i].offset];
    } else {
        return NULL;
    }
}

LLLY_STMT
lllys_snode2stmt(LLLYS_NODE nodetype)
{
    switch(nodetype) {
    case LLLYS_CONTAINER:
        return LLLY_STMT_CONTAINER;
    case LLLYS_CHOICE:
        return LLLY_STMT_CHOICE;
    case LLLYS_LEAF:
        return LLLY_STMT_LEAF;
    case LLLYS_LEAFLIST:
        return LLLY_STMT_LEAFLIST;
    case LLLYS_LIST:
        return LLLY_STMT_LIST;
    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
        return LLLY_STMT_ANYDATA;
    case LLLYS_CASE:
        return LLLY_STMT_CASE;
    case LLLYS_NOTIF:
        return LLLY_STMT_NOTIFICATION;
    case LLLYS_RPC:
        return LLLY_STMT_RPC;
    case LLLYS_INPUT:
        return LLLY_STMT_INPUT;
    case LLLYS_OUTPUT:
        return LLLY_STMT_OUTPUT;
    case LLLYS_GROUPING:
        return LLLY_STMT_GROUPING;
    case LLLYS_USES:
        return LLLY_STMT_USES;
    case LLLYS_AUGMENT:
        return LLLY_STMT_AUGMENT;
    case LLLYS_ACTION:
        return LLLY_STMT_ACTION;
    default:
        return LLLY_STMT_NODE;
    }
}

static struct lllytype_plugin_list *
lllytype_find(const char *module, const char *revision, const char *type_name)
{
    uint16_t u;

    for (u = 0; u < type_plugins_count; ++u) {
        if (llly_strequal(module, type_plugins[u].module, 0) && ((!revision && !type_plugins[u].revision)
                || (revision && llly_strequal(revision, type_plugins[u].revision, 0)))
                && llly_strequal(type_name, type_plugins[u].name, 0)) {
            return &(type_plugins[u]);
        }
    }

    return NULL;
}

int
lllytype_store(const struct lllys_module *mod, const char *type_name, const char **value_str, lllyd_val *value)
{
    struct lllytype_plugin_list *p;
    char *err_msg = NULL;

    assert(mod && type_name && value_str && value);

    p = lllytype_find(mod->name, mod->rev_size ? mod->rev[0].date : NULL, type_name);
    if (p) {
        if (p->store_clb(mod->ctx, type_name, value_str, value, &err_msg)) {
            if (!err_msg) {
                if (asprintf(&err_msg, "Failed to store value \"%s\" of user type \"%s\".", *value_str, type_name) == -1) {
                    LOGMEM(mod->ctx);
                    return -1;
                }
            }
            LOGERR(mod->ctx, LLLY_EPLUGIN, err_msg);
            free(err_msg);
            return -1;
        }

        /* value successfully stored */
        return 0;
    }

    return 1;
}

void
lllytype_free(const struct lllys_type *type, lllyd_val value, const char *value_str)
{
    struct lllytype_plugin_list *p;
    struct lllys_node_leaf sleaf;
    struct lllyd_node_leaf_list leaf;
    struct lllys_module *mod;

    memset(&sleaf, 0, sizeof sleaf);
    memset(&leaf, 0, sizeof leaf);

    while (type->base == LLLY_TYPE_LEAFREF) {
        type = &type->info.lref.target->type;
    }
    if (type->base == LLLY_TYPE_UNION) {
        /* create a fake schema node */
        sleaf.module = type->parent->module;
        sleaf.name = "fake-leaf";
        sleaf.type = *type;
        sleaf.nodetype = LLLYS_LEAF;

        /* and a fake data node */
        leaf.schema = (struct lllys_node *)&sleaf;
        leaf.value = value;
        leaf.value_str = value_str;

        /* find the original type */
        type = lllyd_leaf_type(&leaf);
        if (!type) {
            LOGINT(sleaf.module->ctx);
            return;
        }
    }

    mod = type->der->module;
    if (!mod) {
        LOGINT(type->parent->module->ctx);
        return;
    }

    p = lllytype_find(mod->name, mod->rev_size ? mod->rev[0].date : NULL, type->der->name);
    if (!p) {
        LOGINT(mod->ctx);
        return;
    }

    if (p->free_clb) {
        p->free_clb(value.ptr);
    }
}
