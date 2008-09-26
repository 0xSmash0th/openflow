all_modules = $(dist_modules)
dist_modules = openflow 

openflow_sources = \
	chain.c \
	crc32.c \
	datapath.c \
	dp_dev.c \
	dp_notify.c \
	flow.c \
	forward.c \
	table-hash.c \
	table-linear.c 

openflow_headers = \
	chain.h \
	compat.h \
	crc32.h \
	datapath.h \
	dp_dev.h \
	flow.h \
	forward.h \
	snap.h \
	table.h 

dist_sources = $(foreach module,$(dist_modules),$($(module)_sources))
dist_headers = $(foreach module,$(dist_modules),$($(module)_headers))
all_sources = $(foreach module,$(all_modules),$($(module)_sources))
all_headers = $(foreach module,$(all_modules),$($(module)_headers))
all_links = $(notdir $(all_sources))
all_objects = $(notdir $(patsubst %.c,%.o,$(all_sources)))
