#ifndef __GST_TIME_INSERTER_H__
#define __GST_TIME_INSERTER_H__

#include <gst/gst.h>
G_BEGIN_DECLS

#define GST_TYPE_TIME_INSERTER (gst_time_inserter_get_type())
#define GST_TIME_INSERTER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_TIME_INSERTER, GstTimeInserter))
#define GST_TIME_INSERTER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_TIME_INSERTER, GstTimeInserterClass))
#define GST_IS_TIME_INSERTER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_TIME_INSERTER))
#define GST_IS_TIME_INSERTER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_TIME_INSERTER))

#ifndef NS_PER_S
#define NS_PER_S (1000000000)
#endif

typedef struct _GstTimeInserter GstTimeInserter;
typedef struct _GstTimeInserterClass GstTimeInserterClass;

struct _GstTimeInserter {
  GstElement element;

  GstPad* sinkpad;
  GstPad* srcpad;

  guint64 firstFrameTaiTime;
  guint64 framesProcessed;
};

struct _GstTimeInserterClass {
  GstElementClass parent_class;
};

GType gst_time_inserter_get_type(void);

G_END_DECLS

#endif /* __GST_TIME_INSERTER_H__ */
