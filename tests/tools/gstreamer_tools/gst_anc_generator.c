#include "gst_anc_generator.h"

#include <gst/base/gstbasesrc.h>
#include <gst/gst.h>
#include <stdio.h>

/*
  Example ancillary payload data
  Ancillary packet count: 3
    Ancillary packet 1:
      F bit: 0b00
      C bit: 0b0
      DID: 0x60
      SDID: 0x60: Ancillary Time Code (S12M-2)
      Line number: 9
      Horizontal offset: 0
      S bit: 0b0 (Stream Number not used)
      Stream num: 0
      Data count: 16
      Checksum word: 0x270
    Anc 2:
      F bit: 0b00
      C bit: 0b0
      DID:  0x61
      SDID: 0x01: EIA 708B Data mapping into VANC space (S334-1)
      Line number: 10
      Horizontal offset: 0
      S bit: 0b0 (Stream number not used)
      Stream num: 0
      Data count: 16
      Checksum word: 0x172
    Anc 3:
      F bit: 0b00
      C bit: 0b0
      DID:  0x41
      SDID: 0x07: ANSI/SCTE 104 messages (S2010)
      Line number: 11
      Horizontal offset: 0
      S bit: 0b0 (Stream number not used)
      Stream num: 0
      Data count: 60
      Checksum word: 0x2bd
 **/
static unsigned char ancillary_example[] = {
    0x03, 0x00, 0x00, 0x00, 0x00, 0x90, 0x00, 0x00, 0x98, 0x26, 0x04, 0x41, 0x70, 0x94,
    0x25, 0x08, 0x01, 0x20, 0x60, 0x20, 0x08, 0x01, 0x40, 0x94, 0x25, 0x04, 0x81, 0x10,
    0x80, 0x1E, 0x08, 0x02, 0x70, 0x00, 0x00, 0x00, 0x00, 0xA0, 0x00, 0x00, 0x58, 0x50,
    0x14, 0x42, 0x96, 0x9A, 0x51, 0x05, 0xFD, 0x43, 0x8F, 0x26, 0xA9, 0xCA, 0xE1, 0x7F,
    0x58, 0x06, 0x02, 0x74, 0x8F, 0x26, 0xA4, 0x7D, 0x72, 0x00, 0x00, 0x00, 0x00, 0xE0,
    0x00, 0x00, 0x90, 0x50, 0x78, 0xF1, 0x08, 0xBF, 0xEF, 0xF8, 0x02, 0x3C, 0x80, 0x20,
    0x08, 0x02, 0x00, 0x80, 0x20, 0x08, 0x02, 0x03, 0x40, 0x50, 0x48, 0x01, 0x02, 0x84,
    0x9F, 0x84, 0x06, 0x09, 0x80, 0x20, 0x68, 0x51, 0x04, 0x48, 0xD2, 0xA8, 0xC1, 0x32,
    0x40, 0x50, 0xB8, 0x02, 0x1B, 0x80, 0x20, 0x08, 0x0D, 0xE9, 0x80, 0x1A, 0x45, 0x6D,
    0x01, 0x82, 0x53, 0x14, 0xC5, 0x38, 0x4C, 0x53, 0x24, 0xE2, 0x33, 0x4C, 0x53, 0x28,
    0x89, 0x01, 0x40, 0x60, 0x04, 0x06, 0x00, 0x80, 0x20, 0x08, 0x02, 0xBD};

/*
  Example ancillary payload data
  Ancillary packet count: 1
    Ancillary packet 1:
      F bit: 0b00
      C bit: 0b0
      DID: 0x60
      SDID: 0x60: Ancillary Time Code (S12M-2)
      Line number: 9
      Horizontal offset: 0
      S bit: 0b0 (Stream Number not used)
      Stream num: 0
      Data count: 8
      Checksum word: 0x20c
 **/
static unsigned char ancillary_example2[] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x90, 0x00, 0x00, 0x98, 0x26, 0x04, 0x22,
    0x44, 0x80, 0x20, 0x08, 0x02, 0x00, 0x80, 0x20, 0x08, 0x02, 0x0c, 0x00};

/*
  Example ancillary payload data
  Ancillary packet count: 3
    Ancillary packet 1:
      F bit: 0b00
      C bit: 0b0
      DID: 0x60
      SDID: 0x01: EIA 708B Data mapping into VANC space (S334-1)
      Line number: 9
      Horizontal offset: 0
      S bit: 0b0 (Stream Number not used)
      Stream num: 0
      Data count: 16
      Checksum word: 0x272
    Anc 2:
      F bit: 0b00
      C bit: 0b0
      DID: 0x60
      SDID: 0x60: Ancillary Time Code (S12M-2)
      Line number: 10
      Horizontal offset: 0
      S bit: 0b0 (Stream number not used)
      Stream num: 0
      Data count: 16
      Checksum word: 0x180
    Anc 3:
      F bit: 0b00
      C bit: 0b0
      DID:  0x41
      SDID: 0x07: ANSI/SCTE 104 messages (S2010)
      Line number: 11
      Horizontal offset: 0
      S bit: 0b0 (Stream number not used)
      Stream num: 0
      Data count: 60
      Checksum word: 0x2bd
 **/
static unsigned char ancillary_example3[] = {
    0x03, 0x00, 0x00, 0x00, 0x00, 0x90, 0x00, 0x00, 0x58, 0x50, 0x14, 0x42, 0x96, 0x9A,
    0x51, 0x05, 0xFD, 0x43, 0x57, 0x5B, 0xC9, 0xCA, 0xE1, 0x7F, 0x50, 0x16, 0x16, 0x74,
    0x57, 0x5B, 0xC6, 0xCE, 0x72, 0x00, 0x00, 0x00, 0x00, 0xA0, 0x00, 0x00, 0x98, 0x26,
    0x04, 0x41, 0x70, 0x48, 0x26, 0x04, 0x41, 0x20, 0x60, 0x25, 0x08, 0x02, 0x50, 0x94,
    0x12, 0x04, 0x82, 0x00, 0x80, 0x1E, 0x08, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00, 0xC0,
    0x00, 0x00, 0x90, 0x50, 0x78, 0xF1, 0x08, 0xBF, 0xEF, 0xF8, 0x02, 0x3C, 0x80, 0x20,
    0x08, 0x02, 0x00, 0x80, 0x20, 0x08, 0x02, 0x03, 0x40, 0x50, 0x48, 0x01, 0x02, 0x84,
    0x9F, 0x84, 0x06, 0x09, 0x80, 0x20, 0x68, 0x51, 0x04, 0x48, 0xD2, 0xA8, 0xC1, 0x32,
    0x40, 0x50, 0xB8, 0x02, 0x1B, 0x80, 0x20, 0x08, 0x0D, 0xE9, 0x80, 0x10, 0x14, 0x39,
    0x01, 0x82, 0x53, 0x14, 0xC5, 0x38, 0x4C, 0x63, 0x34, 0xE1, 0x37, 0x4E, 0x13, 0x18,
    0x89, 0x01, 0x40, 0x60, 0x04, 0x06, 0x00, 0x80, 0x20, 0x08, 0x01, 0xD8};

/*
  Example ancillary payload data
  Ancillary packet count: 0
 **/
static unsigned char ancillary_example4[] = {0x00, 0x00, 0x00, 0x00};

static unsigned char* ancillary_packets[] = {ancillary_example, ancillary_example2,
                                             ancillary_example3, ancillary_example4};
static const size_t ancillary_packet_sizes[] = {
    sizeof(ancillary_example), sizeof(ancillary_example2), sizeof(ancillary_example3),
    sizeof(ancillary_example4)};

GST_DEBUG_CATEGORY_STATIC(gst_anc_generator_debug);
#define GST_CAT_DEFAULT gst_anc_generator_debug

#define DEFAULT_NUM_FRAMES 0
#define DEFAULT_FRAMERATE_NUM 25
#define DEFAULT_FRAMERATE_DEN 1

enum { PROP_0, PROP_NUM_FRAMES, PROP_FRAMERATE };

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS("application/x-ancillary-data"));

G_DEFINE_TYPE_WITH_CODE(GstAncGenerator, gst_anc_generator, GST_TYPE_BASE_SRC,
                        GST_DEBUG_CATEGORY_INIT(gst_anc_generator_debug, "anc_generator",
                                                0, "Ancillary data payload generator"));

GST_ELEMENT_REGISTER_DEFINE(GstAncGenerator, "anc_generator", GST_RANK_NONE,
                            GST_TYPE_ANC_GENERATOR);

static void gst_anc_generator_set_property(GObject* object, guint prop_id,
                                           const GValue* value, GParamSpec* pspec);
static void gst_anc_generator_get_property(GObject* object, guint prop_id, GValue* value,
                                           GParamSpec* pspec);
static void gst_anc_generator_dispose(GObject* object);
static void gst_anc_generator_finalize(GObject* object);

static gboolean gst_anc_generator_start(GstBaseSrc* basesrc);
static gboolean gst_anc_generator_stop(GstBaseSrc* basesrc);
static GstFlowReturn gst_anc_generator_create(GstBaseSrc* basesrc, guint64 offset,
                                              guint size, GstBuffer** buf);
static gboolean gst_anc_generator_query(GstBaseSrc* basesrc, GstQuery* query);

static void gst_anc_generator_class_init(GstAncGeneratorClass* klass) {
  GObjectClass* gobject_class = (GObjectClass*)klass;
  GstElementClass* gstelement_class = (GstElementClass*)klass;
  GstBaseSrcClass* gstbasesrc_class = (GstBaseSrcClass*)klass;

  gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_anc_generator_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_anc_generator_get_property);
  gobject_class->dispose = GST_DEBUG_FUNCPTR(gst_anc_generator_dispose);
  gobject_class->finalize = GST_DEBUG_FUNCPTR(gst_anc_generator_finalize);

  gstbasesrc_class->start = GST_DEBUG_FUNCPTR(gst_anc_generator_start);
  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR(gst_anc_generator_stop);
  gstbasesrc_class->create = GST_DEBUG_FUNCPTR(gst_anc_generator_create);
  gstbasesrc_class->query = GST_DEBUG_FUNCPTR(gst_anc_generator_query);

  gst_element_class_add_pad_template(gstelement_class,
                                     gst_static_pad_template_get(&src_factory));

  gst_element_class_set_static_metadata(
      gstelement_class, "ST2110 Ancillary Data Generator", "Source/Metadata",
      "Generates ancillary data payload for ST2110-40 streams",
      "Dawid Wesierski <dawid.wesierski@intel.com>");

  g_object_class_install_property(
      gobject_class, PROP_NUM_FRAMES,
      g_param_spec_uint("num-frames", "Number of frames",
                        "Number of frames to generate (0 = infinite)", 0, G_MAXUINT,
                        DEFAULT_NUM_FRAMES, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_FRAMERATE,
      gst_param_spec_fraction("fps", "framerate", "Framerate", 1, 1, G_MAXINT, 1,
                              DEFAULT_FRAMERATE_NUM, DEFAULT_FRAMERATE_DEN,
                              G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void gst_anc_generator_init(GstAncGenerator* src) {
  src->num_frames = DEFAULT_NUM_FRAMES;
  src->framerate_num = DEFAULT_FRAMERATE_NUM;
  src->framerate_den = DEFAULT_FRAMERATE_DEN;
  src->frames_generated = 0;
  src->running_time = 0;

  gst_base_src_set_live(GST_BASE_SRC(src), FALSE);
  gst_base_src_set_format(GST_BASE_SRC(src), GST_FORMAT_TIME);
}

static void gst_anc_generator_dispose(GObject* object) {
  GstAncGenerator* src = GST_ANC_GENERATOR(object);
  g_return_if_fail(GST_IS_ANC_GENERATOR(src));

  G_OBJECT_CLASS(gst_anc_generator_parent_class)->dispose(object);
}

static void gst_anc_generator_finalize(GObject* object) {
  GstAncGenerator* src = GST_ANC_GENERATOR(object);
  g_return_if_fail(GST_IS_ANC_GENERATOR(src));

  G_OBJECT_CLASS(gst_anc_generator_parent_class)->finalize(object);
}

static void gst_anc_generator_set_property(GObject* object, guint prop_id,
                                           const GValue* value, GParamSpec* pspec) {
  GstAncGenerator* src = GST_ANC_GENERATOR(object);
  g_return_if_fail(GST_IS_ANC_GENERATOR(src));

  switch (prop_id) {
    case PROP_NUM_FRAMES:
      src->num_frames = g_value_get_uint(value);
      break;
    case PROP_FRAMERATE:
      src->framerate_num = gst_value_get_fraction_numerator(value);
      src->framerate_den = gst_value_get_fraction_denominator(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gst_anc_generator_get_property(GObject* object, guint prop_id, GValue* value,
                                           GParamSpec* pspec) {
  GstAncGenerator* src = GST_ANC_GENERATOR(object);
  g_return_if_fail(GST_IS_ANC_GENERATOR(src));

  switch (prop_id) {
    case PROP_NUM_FRAMES:
      g_value_set_uint(value, src->num_frames);
      break;
    case PROP_FRAMERATE:
      gst_value_set_fraction(value, src->framerate_num, src->framerate_den);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static gboolean gst_anc_generator_start(GstBaseSrc* basesrc) {
  GstAncGenerator* src = GST_ANC_GENERATOR(basesrc);

  GST_DEBUG_OBJECT(src, "Starting ancillary data generator");
  src->frames_generated = 0;
  src->running_time = 0;

  return TRUE;
}

static gboolean gst_anc_generator_stop(GstBaseSrc* basesrc) {
  GstAncGenerator* src = GST_ANC_GENERATOR(basesrc);

  GST_DEBUG_OBJECT(src, "Stopping ancillary data generator");

  return TRUE;
}

static gboolean gst_anc_generator_query(GstBaseSrc* basesrc, GstQuery* query) {
  GstAncGenerator* src = GST_ANC_GENERATOR(basesrc);
  gboolean ret = FALSE;

  switch (GST_QUERY_TYPE(query)) {
    case GST_QUERY_DURATION: {
      GstFormat format;
      gst_query_parse_duration(query, &format, NULL);

      if (format == GST_FORMAT_TIME && src->num_frames > 0) {
        GstClockTime duration = gst_util_uint64_scale_int(
            GST_SECOND * src->num_frames, src->framerate_den, src->framerate_num);
        gst_query_set_duration(query, GST_FORMAT_TIME, duration);
        ret = TRUE;
      } else if (format == GST_FORMAT_TIME && src->num_frames == 0) {
        gst_query_set_duration(query, GST_FORMAT_TIME, GST_CLOCK_TIME_NONE);
        ret = TRUE;
      }
      break;
    }
    case GST_QUERY_SEEKING: {
      GstFormat format;
      gst_query_parse_seeking(query, &format, NULL, NULL, NULL);

      if (format == GST_FORMAT_TIME) {
        gst_query_set_seeking(query, GST_FORMAT_TIME, FALSE, 0, -1);
        ret = TRUE;
      }
      break;
    }
    default:
      ret = GST_BASE_SRC_CLASS(gst_anc_generator_parent_class)->query(basesrc, query);
      break;
  }

  return ret;
}

static GstFlowReturn gst_anc_generator_create(GstBaseSrc* basesrc, guint64 offset,
                                              guint size, GstBuffer** buf) {
  GstAncGenerator* src = GST_ANC_GENERATOR(basesrc);

  if (src->num_frames > 0 && src->frames_generated >= src->num_frames) {
    GST_DEBUG_OBJECT(src, "Reached maximum number of frames (%u), sending EOS",
                     src->num_frames);
    return GST_FLOW_EOS;
  }

  const unsigned char* package;
  size_t package_size;

  guint pattern_index =
      src->frames_generated % (sizeof(ancillary_packets) / sizeof(ancillary_packets[0]));
  package = ancillary_packets[pattern_index];
  package_size = ancillary_packet_sizes[pattern_index];

  *buf = gst_buffer_new_allocate(NULL, package_size, NULL);
  if (!*buf) {
    GST_ERROR_OBJECT(src, "Failed to allocate buffer");
    return GST_FLOW_ERROR;
  }

  gst_buffer_fill(*buf, 0, package, package_size);
  GstClockTime frame_duration =
      gst_util_uint64_scale_int(GST_SECOND, src->framerate_den, src->framerate_num);

  GST_BUFFER_PTS(*buf) = src->running_time;
  GST_BUFFER_DTS(*buf) = src->running_time;
  GST_BUFFER_DURATION(*buf) = frame_duration;

  src->running_time += frame_duration;
  src->frames_generated++;

  GST_DEBUG_OBJECT(src,
                   "Generated frame %u with pattern %u, size %zu bytes, PTS "
                   "%" GST_TIME_FORMAT,
                   src->frames_generated, pattern_index, package_size,
                   GST_TIME_ARGS(GST_BUFFER_PTS(*buf)));

  return GST_FLOW_OK;
}

#ifndef GST_CAT_DEFAULT
#define GST_CAT_DEFAULT gst_anc_generator_debug
#endif

#ifndef GST_LICENSE
#define GST_LICENSE "LGPL"
#endif
#ifndef GST_API_VERSION
#define GST_API_VERSION "1.0"
#endif
#ifndef GST_PACKAGE_NAME
#define GST_PACKAGE_NAME "ancillary data IETF 8331 payload generator"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/OpenVisualCloud/Media-Transport-Library"
#endif
#ifndef PACKAGE
#define PACKAGE "ancgenerator"
#endif
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "1.0"
#endif
#ifndef PLUGIN_DBG_DESC
#define PLUGIN_DBG_DESC "ancillary data IETF 8331 payload generator"
#endif

static gboolean plugin_init(GstPlugin* plugin) {
  GST_DEBUG_CATEGORY_INIT(gst_anc_generator_debug, "ancgenerator", 0, PLUGIN_DBG_DESC);

  if (!gst_element_register(plugin, "ancgenerator", GST_RANK_NONE,
                            GST_TYPE_ANC_GENERATOR))
    return FALSE;

  return TRUE;
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, ancgenerator,
                  "ancillary data IETF 8331 payload generator", plugin_init,
                  PACKAGE_VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)