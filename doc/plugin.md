# Plugin Guide

## 1. Introduction:
Kahawai introduce plugin lib support from v22.06. With plugin, it's possible to add 3rd party add-ons to the running system, ex a CPU/GPU/FPGA based jpegxs codec, a color space format converting acceleration, or others.

Kahawai will try to load plugin libs during the st_init routine, it will search the plugins from the user json config file and parse the plugin version and symbol. If all the sanity check is fine, Kahawai will add the plugin to the list and call the create function which exported by the plugin lib. A plugin should do all the register work in the create function, and do the release in the free function.

## 2. Plugin APIs:
A plugin should implement 3 functions, st_plugin_get_meta, st_plugin_create and st_plugin_free, see below for the detail.
```bash
/** Get meta function porotype of plugin */
typedef int (*st_plugin_get_meta_fn)(struct st_plugin_meta* meta);
/** Get meta function name of plugin */
#define ST_PLUGIN_GET_META_API "st_plugin_get_meta"
/** Create function porotype of plugin */
typedef st_plugin_priv (*st_plugin_create_fn)(st_handle st);
/** Create function name of plugin */
#define ST_PLUGIN_CREATE_API "st_plugin_create"
/** Free function porotype of plugin */
typedef int (*st_plugin_free_fn)(st_plugin_priv handle);
/** Free function name of plugin */
#define ST_PLUGIN_FREE_API "st_plugin_free"
```

## 3. Sample:
Refer to [plugins sample](../plugins/sample) for how to build and create a plugin.

## 4. Plugin enable:
Refer to [kahawai.json](../kahawai.json) for how to register a plugin in the config.

The default json config file path is kahawai.json, if you want to parse from a different file, pls pass KAHAWAI_CFG_PATH env to the process.
```bash
export KAHAWAI_CFG_PATH=your_json_config_file_path
```
