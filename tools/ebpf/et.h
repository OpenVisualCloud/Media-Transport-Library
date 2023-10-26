#ifndef __ET_H
#define __ET_H

struct udp_send_event {
  int pid;
  int udp_send_cnt;
  unsigned int gso_size;
  unsigned long long duration_ns;
  unsigned int udp_send_bytes;
  int ret;
};

#endif /* __ET_H */