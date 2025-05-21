#ifndef __GST_ANC_INSERTER_H__
#define __GST_ANC_INSERTER_H__

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_ANC_INSERTER (gst_anc_inserter_get_type())
#define GST_ANC_INSERTER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_ANC_INSERTER, GstAncInserter))
#define GST_ANC_INSERTER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_ANC_INSERTER, GstAncInserterClass))
#define GST_IS_ANC_INSERTER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_ANC_INSERTER))
#define GST_IS_ANC_INSERTER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_ANC_INSERTER))

typedef struct _GstAncInserter GstAncInserter;
typedef struct _GstAncInserterClass GstAncInserterClass;

struct _GstAncInserter {
  GstElement element;

  // pads
  GstPad* sinkpad;
  GstPad* srcpad;

  // properties
  gboolean unused;  // placeholder for future use

  // other state
  guint64 frameRateNumerator;  // cache of the frame rate 24, 25, 30, 50, 60
  guint64 framesProcessed;     // number of frames we've processed
  gboolean isDropFrame;        // is our timecode drop frame
};

struct _GstAncInserterClass {
  GstElementClass parent_class;
};

GType gst_anc_inserter_get_type(void);

G_END_DECLS

#endif /* __GST_ANC_INSERTER_H__ */
