#include "gst_time_inserter.h"

#include <gst/gst.h>
#include <stdio.h>

GST_DEBUG_CATEGORY_STATIC(gst_time_inserter_debug);
#define GST_CAT_DEFAULT gst_time_inserter_debug

enum {
  PROP_0, /* required as first item */
  PROP_UNUSED
};

static GstStaticPadTemplate sink_factory =
    GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
                            GST_STATIC_CAPS("video/x-raw, "
                                            "format = (string) {v210, I422_10LE},"
                                            "width = (int) [64, 16384], "
                                            "height = (int) [64, 8704], "
                                            "framerate = (fraction) [1, MAX]"));

static GstStaticPadTemplate src_factory =
    GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS,
                            GST_STATIC_CAPS("video/x-raw, "
                                            "format = (string) {v210, I422_10LE},"
                                            "width = (int) [64, 16384], "
                                            "height = (int) [64, 8704], "
                                            "framerate = (fraction) [1, MAX]"));

G_DEFINE_TYPE_WITH_CODE(GstTimeInserter, gst_time_inserter, GST_TYPE_ELEMENT,
                        GST_DEBUG_CATEGORY_INIT(gst_time_inserter_debug, "time_inserter",
                                                0, "time_inserter"));

GST_ELEMENT_REGISTER_DEFINE(GstTimeInserter, "time_inserter", GST_RANK_NONE,
                            GST_TYPE_TIME_INSERTER);

static void gst_time_inserter_set_property(GObject* object, guint prop_id,
                                           const GValue* value, GParamSpec* pspec);
static void gst_time_inserter_get_property(GObject* object, guint prop_id, GValue* value,
                                           GParamSpec* pspec);
static void gst_time_inserter_dispose(GObject* object);
static void gst_time_inserter_finalize(GObject* object);

static gboolean gst_time_inserter_sink_event(GstPad* pad, GstObject* parent,
                                             GstEvent* event);
static gboolean gst_time_inserter_sink_query(GstPad* pad, GstObject* parent,
                                             GstQuery* query);
static gboolean gst_time_inserter_src_query(GstPad* pad, GstObject* parent,
                                            GstQuery* query);
static GstFlowReturn gst_time_inserter_chain(GstPad* pad, GstObject* parent,
                                             GstBuffer* buf);

static void gst_time_inserter_class_init(GstTimeInserterClass* klass) {
  GObjectClass* gobject_class = (GObjectClass*)klass;
  GstElementClass* gstelement_class = (GstElementClass*)klass;

  gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_time_inserter_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_time_inserter_get_property);
  gobject_class->dispose = GST_DEBUG_FUNCPTR(gst_time_inserter_dispose);
  gobject_class->finalize = GST_DEBUG_FUNCPTR(gst_time_inserter_finalize);

  gst_element_class_add_pad_template(gstelement_class,
                                     gst_static_pad_template_get(&src_factory));
  gst_element_class_add_pad_template(gstelement_class,
                                     gst_static_pad_template_get(&sink_factory));

  gst_element_class_set_static_metadata(
      gstelement_class, "tai time inserter", "Filter/Converter/Video",
      "Enables automatic user time control by inserting TAI time into video "
      "frames",
      "Dawid Wesierski <dawid.wesierski@intel.com>");
}

static void gst_time_inserter_init(GstTimeInserter* filter) {
  filter->sinkpad = gst_pad_new_from_static_template(&sink_factory, "sink");
  gst_pad_set_event_function(filter->sinkpad,
                             GST_DEBUG_FUNCPTR(gst_time_inserter_sink_event));
  gst_pad_set_query_function(filter->sinkpad,
                             GST_DEBUG_FUNCPTR(gst_time_inserter_sink_query));
  gst_pad_set_chain_function(filter->sinkpad, GST_DEBUG_FUNCPTR(gst_time_inserter_chain));
  GST_PAD_SET_PROXY_CAPS(filter->sinkpad);
  gst_element_add_pad(GST_ELEMENT(filter), filter->sinkpad);

  filter->srcpad = gst_pad_new_from_static_template(&src_factory, "src");
  gst_pad_set_query_function(filter->srcpad,
                             GST_DEBUG_FUNCPTR(gst_time_inserter_src_query));
  GST_PAD_SET_PROXY_CAPS(filter->srcpad);
  gst_element_add_pad(GST_ELEMENT(filter), filter->srcpad);

  filter->framesProcessed = 0;
}

static void gst_time_inserter_dispose(GObject* object) {
  GstTimeInserter* filter = GST_TIME_INSERTER(object);
  g_return_if_fail(GST_IS_TIME_INSERTER(filter));

  G_OBJECT_CLASS(gst_time_inserter_parent_class)->dispose(object);
}

static void gst_time_inserter_finalize(GObject* object) {
  GstTimeInserter* filter = GST_TIME_INSERTER(object);
  g_return_if_fail(GST_IS_TIME_INSERTER(filter));
  G_OBJECT_CLASS(gst_time_inserter_parent_class)->finalize(object);
}

static void gst_time_inserter_set_property(GObject* object, guint prop_id,
                                           const GValue* value, GParamSpec* pspec) {
  GstTimeInserter* filter = GST_TIME_INSERTER(object);
  g_return_if_fail(GST_IS_TIME_INSERTER(filter));

  switch (prop_id) {
    case PROP_UNUSED:
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gst_time_inserter_get_property(GObject* object, guint prop_id, GValue* value,
                                           GParamSpec* pspec) {
  GstTimeInserter* filter = GST_TIME_INSERTER(object);
  g_return_if_fail(GST_IS_TIME_INSERTER(filter));

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static gboolean gst_time_inserter_sink_event(GstPad* pad, GstObject* parent,
                                             GstEvent* event) {
  GstTimeInserter* filter = GST_TIME_INSERTER(parent);
  g_return_val_if_fail(GST_IS_TIME_INSERTER(filter), FALSE);
  gboolean ret = FALSE;

  GST_LOG_OBJECT(filter, "Received %s event on sink pad: %" GST_PTR_FORMAT,
                 GST_EVENT_TYPE_NAME(event), event);

  switch (GST_EVENT_TYPE(event)) {
    default:
      ret = gst_pad_event_default(pad, parent, event);
      break;
  }
  return ret;
}

static gboolean gst_time_inserter_sink_query(GstPad* pad, GstObject* parent,
                                             GstQuery* query) {
  GstTimeInserter* filter = GST_TIME_INSERTER(parent);
  g_return_val_if_fail(GST_IS_TIME_INSERTER(filter), FALSE);
  gboolean ret = FALSE;

  GST_LOG_OBJECT(filter, "Received %s query on sink pad: %" GST_PTR_FORMAT,
                 GST_QUERY_TYPE_NAME(query), query);

  switch (GST_QUERY_TYPE(query)) {
    default:
      ret = gst_pad_query_default(pad, parent, query);
      break;
  }
  return ret;
}

static gboolean gst_time_inserter_src_query(GstPad* pad, GstObject* parent,
                                            GstQuery* query) {
  GstTimeInserter* filter = GST_TIME_INSERTER(parent);
  g_return_val_if_fail(GST_IS_TIME_INSERTER(filter), FALSE);
  gboolean ret = FALSE;

  GST_LOG_OBJECT(filter, "Received %s query on src pad: %" GST_PTR_FORMAT,
                 GST_QUERY_TYPE_NAME(query), query);

  switch (GST_QUERY_TYPE(query)) {
    case GST_QUERY_CAPS:
      ret = gst_pad_query_default(pad, parent, query);
      break;

    case GST_QUERY_ACCEPT_CAPS:
      ret = gst_pad_query_default(pad, parent, query);
      break;

    default:
      ret = gst_pad_query_default(pad, parent, query);
      break;
  }
  return ret;
}

static GstFlowReturn gst_time_inserter_chain(GstPad* pad, GstObject* parent,
                                             GstBuffer* buffer) {
  GstTimeInserter* filter = GST_TIME_INSERTER(parent);
  struct timespec ts;
  guint64 pts_time = GST_BUFFER_PTS(buffer);
  guint64 tai_time;

  filter->framesProcessed++;

  if (!filter->firstFrameTaiTime) {
    if (clock_gettime(CLOCK_TAI, &ts) != 0) {
      GST_ERROR("Failed to get TAI time");
      return GST_FLOW_ERROR;
    }

    filter->firstFrameTaiTime = (((guint64)ts.tv_sec) * NS_PER_S + ts.tv_nsec) - pts_time;
    GST_INFO("Captured first frame TAI time: %lu, PTS: %ld\n", filter->firstFrameTaiTime,
             pts_time);
  }

  tai_time = filter->firstFrameTaiTime + pts_time;

  if (clock_gettime(CLOCK_TAI, &ts) != 0) {
    GST_ERROR("Failed to get TAI time");
    return GST_FLOW_ERROR;
  }

  GST_BUFFER_PTS(buffer) = tai_time;
  return gst_pad_push(filter->srcpad, buffer);
}

#ifndef GST_CAT_DEFAULT
#define GST_CAT_DEFAULT gst_time_inserter_debug
#endif

#ifndef GST_LICENSE
#define GST_LICENSE "LGPL"
#endif
#ifndef GST_API_VERSION
#define GST_API_VERSION "1.0"
#endif
#ifndef GST_PACKAGE_NAME
#define GST_PACKAGE_NAME "Time tai inserter"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/OpenVisualCloud/Media-Transport-Library"
#endif
#ifndef PACKAGE
#define PACKAGE "timeinserter"
#endif
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "1.0"
#endif
#ifndef PLUGIN_DBG_DESC
#define PLUGIN_DBG_DESC "Time inserter for tai"
#endif

static gboolean plugin_init(GstPlugin* plugin) {
  GST_DEBUG_CATEGORY_INIT(gst_time_inserter_debug, PACKAGE, 0, PLUGIN_DBG_DESC);

  return gst_element_register(plugin, PACKAGE, GST_RANK_NONE, GST_TYPE_TIME_INSERTER);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, timeinserter,
                  "Don't worry about it ", plugin_init, PACKAGE_VERSION, GST_LICENSE,
                  GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)