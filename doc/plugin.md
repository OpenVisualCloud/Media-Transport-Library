# Plugin Guide

## 1. Introduction

Starting from version 22.06, the Media Transport Library introduced plugin library support. With plugins, it's possible to add third-party add-ons to the running system, such as a CPU/GPU/FPGA-based JPEG-XS codec, a color space format conversion acceleration, or other functionalities.

The library will attempt to load plugin libraries during the mtl_init routine by searching for plugins in the user JSON config file and parsing the plugin version and symbol. If all the sanity checks are fine, the plugin is added to the list, and the create function exported by the plugin library is called.

A plugin should do all the registration work in the create function and release any resources in the free function.

## 2. Plugin APIs

A plugin should implement three functions: st_plugin_get_meta, st_plugin_create, and st_plugin_free. Please see below for details.

```bash
/** Get meta function prototype of plugin */
typedef int (*st_plugin_get_meta_fn)(struct st_plugin_meta* meta);
/** Get meta function name of plugin */
#define ST_PLUGIN_GET_META_API "st_plugin_get_meta"
/** Create function prototype of plugin */
typedef st_plugin_priv (*st_plugin_create_fn)(st_handle st);
/** Create function name of plugin */
#define ST_PLUGIN_CREATE_API "st_plugin_create"
/** Free function prototype of plugin */
typedef int (*st_plugin_free_fn)(st_plugin_priv handle);
/** Free function name of plugin */
#define ST_PLUGIN_FREE_API "st_plugin_free"
```

## 3. Sample

Refer to [plugins sample](../plugins/sample) for how to build and create a plugin.

## 4. Plugin enable

Refer to [kahawai.json](../kahawai.json) for how to register a plugin in the config.

The default JSON config file path is kahawai.json. If you want to parse from a different file, please pass the KAHAWAI_CFG_PATH environment variable to the process.

```bash
export KAHAWAI_CFG_PATH=your_json_config_file_path
```
