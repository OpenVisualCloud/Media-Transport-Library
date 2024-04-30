.PHONY: clean

CFLAGS  := -Wall -g
LDLIBS  := ${LDLIBS} -lrdmacm -libverbs -lpthread -lmtl_rdma

APPS    := tx rx

all: ${APPS}


clean:
	rm -f ${APPS}
