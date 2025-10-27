#ifndef __GST_ANC_GENERATOR_H__
#define __GST_ANC_GENERATOR_H__

#include <gst/base/gstbasesrc.h>
#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_ANC_GENERATOR (gst_anc_generator_get_type())
G_DECLARE_FINAL_TYPE(GstAncGenerator, gst_anc_generator, GST, ANC_GENERATOR, GstBaseSrc)

struct _GstAncGenerator {
  GstBaseSrc element;
  GstPad* srcpad;

  /* Number of frames to generate (0 = infinite) */
  guint num_frames;
  guint framerate_num;
  guint framerate_den;

  guint frames_generated;
  GstClockTime running_time;
};

struct _GstAncGeneratorClass {
  GstElementClass parent_class;
};

GType gst_anc_generator_get_type(void);

G_END_DECLS

#endif /* __GST_ANC_GENERATOR_H__ */
