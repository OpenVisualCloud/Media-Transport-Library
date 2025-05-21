#include <gst/gst.h>
#include <stdio.h>
//#include "gstbamtechvideometa.h"
//#include "timecodeUtility.h"
//#include "timecodeCString.h"
#include "gst_anc_inserter.h"

#define FULL8331DEMO 1
#ifdef FULL8331DEMO
// canned AFD payload. This is a single
// message (sorry ran out of time to do 2)
// built into the body of the 8331 payload
// starting with the ANC_count byte and ends
// with a word align byte padding to make an
// even 32 bit words. This message was
// created mostly by hand and untested, so maybe
// something not quite right with it. Ah yeah,
// it's got the wrong DID/SDID of 0x60/0x60 instead
// of 0x41/0x05 crap.
static unsigned char cannedAFD[] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x90, 0x00, 0x00,
                                    /*anc cnt F   reserved*//*c ln hori off s streamNum */
                                    0x98, 0x26, 0x04, 0x22, 0x44, 0x80, 0x20, 0x08,
                                  /* DID SDID Data_count  */
                                    0x02, 0x00, 0x80, 0x20, 0x08, 0x02, 0x0c, 0x00};
#else

// canned AFD simple message to try with
// existing intel 40p_tx element. This assumes
// DID = 0x41 SDID = 0x05
static unsigned char cannedAFD[] = {0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
#endif

GST_DEBUG_CATEGORY_STATIC(gst_anc_inserter_debug);
#define GST_CAT_DEFAULT gst_anc_inserter_debug

// Properties this element exposes
enum {
  PROP_0, /* required as first item */
  PROP_UNUSED
};

// Default/Min/Max values for all propreties
#define PROP_UNUSED_DEFAULT FALSE

// Pad templates
static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS("video/x-raw"));

static GstStaticPadTemplate src_factory =
    GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);

// Define our plugin GType
//#define gst_anc_inserter_parent_class parent_class
// G_DEFINE_TYPE(GstAncInserter, gst_anc_inserter, GST_TYPE_ELEMENT);
G_DEFINE_TYPE_WITH_CODE(
    GstAncInserter, gst_anc_inserter, GST_TYPE_ELEMENT,
    GST_DEBUG_CATEGORY_INIT(gst_anc_inserter_debug, "anc_inserter", 0,
                            "Ancillary data IETF 8331 payload generator"));

GST_ELEMENT_REGISTER_DEFINE(GstAncInserter, "anc_inserter", GST_RANK_NONE,
                            GST_TYPE_ANC_INSERTER);

// Base class virtual methods that we override
static void gst_anc_inserter_set_property(GObject* object, guint prop_id,
                                          const GValue* value, GParamSpec* pspec);
static void gst_anc_inserter_get_property(GObject* object, guint prop_id, GValue* value,
                                          GParamSpec* pspec);
static void gst_anc_inserter_dispose(GObject* object);
static void gst_anc_inserter_finalize(GObject* object);

// Pad functions for events, queries, and buffers
static gboolean gst_anc_inserter_sink_event(GstPad* pad, GstObject* parent,
                                            GstEvent* event);
static gboolean gst_anc_inserter_sink_query(GstPad* pad, GstObject* parent,
                                            GstQuery* query);
static gboolean gst_anc_inserter_src_query(GstPad* pad, GstObject* parent,
                                           GstQuery* query);
static GstFlowReturn gst_anc_inserter_chain(GstPad* pad, GstObject* parent,
                                            GstBuffer* buf);

// Initialize our plugin class
static void gst_anc_inserter_class_init(GstAncInserterClass* klass) {
  GObjectClass* gobject_class = (GObjectClass*)klass;
  GstElementClass* gstelement_class = (GstElementClass*)klass;

  gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_anc_inserter_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_anc_inserter_get_property);
  gobject_class->dispose = GST_DEBUG_FUNCPTR(gst_anc_inserter_dispose);
  gobject_class->finalize = GST_DEBUG_FUNCPTR(gst_anc_inserter_finalize);

  // boolean is drop frame timecode
  g_object_class_install_property(
      gobject_class, PROP_UNUSED,
      g_param_spec_boolean("unusedplaceholder", "unused placeholder",
                           "this is a placeholder not used yet", PROP_UNUSED_DEFAULT,
                           (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  // Add pad templates
  gst_element_class_add_pad_template(gstelement_class,
                                     gst_static_pad_template_get(&src_factory));
  gst_element_class_add_pad_template(gstelement_class,
                                     gst_static_pad_template_get(&sink_factory));

  // For Category/MediaType list, see:
  //  https://cgit.freedesktop.org/gstreamer/gstreamer/tree/docs/design/draft-klass.txt
  //
  gst_element_class_set_static_metadata(
      gstelement_class, "ST2110 Ancillary Data Inserter", "Formatter/Metadata",
      "This plugin prototype generates RFC 8331 Ancillary Data payload for each incoming "
      "video frame",
      "Chris Akers <christopher.akers@disney.com>");
}

// Initialize a plugin instance
static void gst_anc_inserter_init(GstAncInserter* filter) {
  // Setup sink pad
  filter->sinkpad = gst_pad_new_from_static_template(&sink_factory, "sink");
  gst_pad_set_event_function(filter->sinkpad,
                             GST_DEBUG_FUNCPTR(gst_anc_inserter_sink_event));
  gst_pad_set_query_function(filter->sinkpad,
                             GST_DEBUG_FUNCPTR(gst_anc_inserter_sink_query));
  gst_pad_set_chain_function(filter->sinkpad, GST_DEBUG_FUNCPTR(gst_anc_inserter_chain));
  GST_PAD_SET_PROXY_CAPS(filter->sinkpad);
  gst_element_add_pad(GST_ELEMENT(filter), filter->sinkpad);

  // Setup src pad
  filter->srcpad = gst_pad_new_from_static_template(&src_factory, "src");
  gst_pad_set_query_function(filter->srcpad,
                             GST_DEBUG_FUNCPTR(gst_anc_inserter_src_query));
  GST_PAD_SET_PROXY_CAPS(filter->srcpad);
  gst_element_add_pad(GST_ELEMENT(filter), filter->srcpad);

  // Initialize properties and other state (note: the struct is zeroed on create)
  filter->unused = PROP_UNUSED_DEFAULT;

  // since our default is to use the source timecode, default
  // internal clock to false. If user sets acq mode, we'll flip it
  // when the property is set
  filter->frameRateNumerator = 60;
  filter->framesProcessed = 0;
  filter->isDropFrame = TRUE;  // normally our timecode would be drop frame
}

static void gst_anc_inserter_dispose(GObject* object) {
  GstAncInserter* filter = GST_ANC_INSERTER(object);
  g_return_if_fail(GST_IS_ANC_INSERTER(filter));

  // 1. Must release all Gst objects that we hold a reference to.
  // 2. dispose() may be called twice; take care to avoid double-free.
  // 3. Our object methods may be called after dispose(); we should not crash.

  // Always chain up last
  G_OBJECT_CLASS(gst_anc_inserter_parent_class)->dispose(object);
}

static void gst_anc_inserter_finalize(GObject* object) {
  GstAncInserter* filter = GST_ANC_INSERTER(object);
  g_return_if_fail(GST_IS_ANC_INSERTER(filter));

  // Free any resources that were not freed in dispose()

  // Always chain up last
  G_OBJECT_CLASS(gst_anc_inserter_parent_class)->finalize(object);
}

static void gst_anc_inserter_set_property(GObject* object, guint prop_id,
                                          const GValue* value, GParamSpec* pspec) {
  GstAncInserter* filter = GST_ANC_INSERTER(object);
  g_return_if_fail(GST_IS_ANC_INSERTER(filter));

  switch (prop_id) {
    case PROP_UNUSED:
      // gboolean dummy = g_value_get_boolean(value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gst_anc_inserter_get_property(GObject* object, guint prop_id, GValue* value,
                                          GParamSpec* pspec) {
  GstAncInserter* filter = GST_ANC_INSERTER(object);
  g_return_if_fail(GST_IS_ANC_INSERTER(filter));

  switch (prop_id) {
    case PROP_UNUSED:
      g_value_set_boolean(value, PROP_UNUSED_DEFAULT);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static gboolean gst_anc_inserter_sink_event(GstPad* pad, GstObject* parent,
                                            GstEvent* event) {
  GstAncInserter* filter = GST_ANC_INSERTER(parent);
  g_return_val_if_fail(GST_IS_ANC_INSERTER(filter), FALSE);
  gboolean ret = FALSE;

  GST_LOG_OBJECT(filter, "Received %s event on sink pad: %" GST_PTR_FORMAT,
                 GST_EVENT_TYPE_NAME(event), event);

  switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_CAPS: {
      GstCaps* caps;
      gint rateDenom, rateNum;  // throw away
      gst_event_parse_caps(event, &caps);

      // get framerate of sink pad
      // gint numer = 0, denom = 1, width = 0, height = 0;
      GstStructure* s = gst_caps_get_structure(caps, 0);
      gst_structure_get_fraction(s, "framerate", &rateNum, &rateDenom);

      // take numerator to a number that looks like 30 or 60 rather
      // than 30000 or 60000
      if (rateNum >= 1000) {
        filter->frameRateNumerator = rateNum / 1000;
      } else {
        filter->frameRateNumerator = rateNum;
      }

      if ((filter->frameRateNumerator != 60) && (filter->frameRateNumerator != 30) &&
          (filter->frameRateNumerator != 50) && (filter->frameRateNumerator != 25) &&
          (filter->frameRateNumerator != 24) && (filter->frameRateNumerator != 48)) {
        // assume it's 60... should never get here.
        GST_ELEMENT_WARNING(GST_ELEMENT_CAST(filter), CORE, PAD, (NULL),
                            ("Unexpected frame rate numerator"));
        filter->frameRateNumerator = 60;
      }

      // really simplistic determination of drop frame timecode.
      // NOTE - in theory drop frame timecode doesn't exist for 23.98,
      // so add a check here to prevent setting it. Not 100% sure about this...
      // since it doesn't make sense that you can have a 23.98 frame rate
      // but not use drop frame timecode.
      if ((rateDenom == 1001) && (filter->frameRateNumerator > 25)) {
        filter->isDropFrame = TRUE;
      } else {
        filter->isDropFrame = FALSE;
      }

      ret = gst_pad_event_default(pad, parent, event);
      break;
    }

    case GST_EVENT_EOS:
      // Sink elements should post EOS after receiving EOS event on ALL sink pads
      // gst_element_post_message(GST_ELEMENT(filter),
      // gst_message_new_eos(GST_OBJECT(filter)));
      ret = gst_pad_event_default(pad, parent, event);
      break;

    default:
      ret = gst_pad_event_default(pad, parent, event);
      break;
  }
  return ret;
}

static gboolean gst_anc_inserter_sink_query(GstPad* pad, GstObject* parent,
                                            GstQuery* query) {
  GstAncInserter* filter = GST_ANC_INSERTER(parent);
  g_return_val_if_fail(GST_IS_ANC_INSERTER(filter), FALSE);
  gboolean ret = FALSE;

  GST_LOG_OBJECT(filter, "Received %s query on sink pad: %" GST_PTR_FORMAT,
                 GST_QUERY_TYPE_NAME(query), query);

  switch (GST_QUERY_TYPE(query)) {
    case GST_QUERY_CAPS:
      // Return the allowable caps for our sink pad. The allowable caps may change
      // depending on the element's state and the element's properties.
      ret = gst_pad_query_default(pad, parent, query);
      break;

    case GST_QUERY_ACCEPT_CAPS:
      // Parse caps and set a true/false result in the event indicating whether
      // we can accept these caps on our sink pad.
      ret = gst_pad_query_default(pad, parent, query);
      break;

    case GST_QUERY_ALLOCATION:
      // Return the allocator(s) that this element implements. May be none.
      ret = gst_pad_query_default(pad, parent, query);
      break;

    default:
      ret = gst_pad_query_default(pad, parent, query);
      break;
  }
  return ret;
}

static gboolean gst_anc_inserter_src_query(GstPad* pad, GstObject* parent,
                                           GstQuery* query) {
  GstAncInserter* filter = GST_ANC_INSERTER(parent);
  g_return_val_if_fail(GST_IS_ANC_INSERTER(filter), FALSE);
  gboolean ret = FALSE;

  GST_LOG_OBJECT(filter, "Received %s query on src pad: %" GST_PTR_FORMAT,
                 GST_QUERY_TYPE_NAME(query), query);

  switch (GST_QUERY_TYPE(query)) {
    case GST_QUERY_CAPS:
      // Return the allowable caps for our src pad. The allowable caps may change
      // depending on the element's state and the element's properties.
      ret = gst_pad_query_default(pad, parent, query);
      break;

    case GST_QUERY_ACCEPT_CAPS:
      // Parse caps and set a true/false result in the event indicating whether
      // we can accept these caps on our src pad.
      ret = gst_pad_query_default(pad, parent, query);
      break;

    default:
      ret = gst_pad_query_default(pad, parent, query);
      break;
  }
  return ret;
}

static GstFlowReturn gst_anc_inserter_chain(GstPad* pad, GstObject* parent,
                                            GstBuffer* buffer) {
  GstAncInserter* filter = GST_ANC_INSERTER(parent);
  g_return_val_if_fail(GST_IS_ANC_INSERTER(filter), GST_FLOW_ERROR);

  // increment our frame count
  filter->framesProcessed++;

  // allocate new 8331 buffer to push to our ouput
  GstBuffer* ancBuffer = gst_buffer_new_allocate(NULL, sizeof(cannedAFD), NULL);

  // copy our AFD user data words into the GST buffer
  gst_buffer_fill(ancBuffer, 0, (gconstpointer)&cannedAFD[0], sizeof(cannedAFD));

  // assign the pts of the ancillary data buffer to be same as incoming video buffer
  GST_BUFFER_PTS(ancBuffer) = GST_BUFFER_PTS(buffer);

  // after we're done extracting whatever metadata we need from
  // the incoming video buffer, unref the incoming video buffer
  gst_buffer_unref(buffer);

  // push the ancillary data buffer
  return gst_pad_push(filter->srcpad, ancBuffer);
}

// element registration done in gst_mtl_plugin.c

//-----------------------------------------------------------------------------
// Initialization for the plugin binary
//

// descriptions for the plugin
#ifndef GST_CAT_DEFAULT
#define GST_CAT_DEFAULT gst_anc_inserter_debug
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
#define PACKAGE "ancinserter"
#endif
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "1.0"
#endif
#ifndef PLUGIN_DBG_DESC
#define PLUGIN_DBG_DESC "ancillary data IETF 8331 payload generator"
#endif

static gboolean plugin_init(GstPlugin* plugin) {
  // export GST_DEBUG=ancinserter:5 to see logs > level 5 for this element
  GST_DEBUG_CATEGORY_INIT(gst_anc_inserter_debug, "ancinserter", 0, PLUGIN_DBG_DESC);

  if (!gst_element_register(plugin, "ancinserter", GST_RANK_NONE, GST_TYPE_ANC_INSERTER))
    return FALSE;

  return TRUE;
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, ancinserter,
                  "ancillary data IETF 8331 payload generator", plugin_init,
                  PACKAGE_VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)