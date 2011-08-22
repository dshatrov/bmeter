COMMON_CFLAGS =				\
	-ggdb				\
	-Wno-long-long -Wall -Wextra	\
	$(THIS_CFLAGS)

COMMON_CFLAGS += -rdynamic

AM_CFLAGS = -std=gnu99 $(COMMON_CFLAGS)
AM_CXXFLAGS = -std=gnu++0x $(COMMON_CFLAGS)

bin_PROGRAMS = bmeter_srv bmeter_clnt

bmeter_srv_SOURCES = server.cpp
bmeter_srv_LDADD = $(THIS_LIBS)
bmeter_srv_LDFLAGS = -Wl,-rpath,'$(libdir)'

bmeter_clnt_SOURCES = client.cpp
bmeter_clnt_LDADD = $(THIS_LIBS)
bmeter_clnt_LDFLAGS = -Wl,-rpath,'$(libdir)'

