## Process this file with automake to produce Makefile.in

DIST_SUBDIRS = \
	zbxdbcache \
	zbxdbhigh \
	zbxdb \
	zbxmemory \
	zbxalgo \
	zbxmedia \
	zbxcrypto \
	zbxcommon \
	zbxcomms \
	zbxcommshigh \
	zbxlog \
	zbxnix \
	zbxconf \
	zbxsysinfo \
	zbxsys \
	zbxjson \
	zbxserver \
	zbxicmpping \
	zbxexec \
	zbxself \
	queue

if PROXY
PROXY_SUBDIRS = \
	zbxcommshigh \
	zbxdb \
	zbxdbcache \
	zbxdbhigh \
	zbxmemory \
	zbxserver \
	zbxicmpping \
	zbxself
endif

if SERVER
SERVER_SUBDIRS = \
	zbxcommshigh \
	zbxdb \
	zbxdbcache \
	zbxdbhigh \
	zbxmemory \
	zbxserver \
	zbxicmpping \
	zbxmedia \
	zbxself
endif

if QUEUE
QUEUE_SUBDIRS = \
	queue
endif

SUBDIRS = \
	zbxcrypto \
	zbxcommon \
	zbxlog \
	zbxalgo \
	zbxnix \
	zbxconf \
	zbxsysinfo \
	zbxsys \
	zbxcomms \
	zbxjson \
	zbxexec \
	$(QUEUE_SUBDIRS) \
	$(PROXY_SUBDIRS) \
	$(SERVER_SUBDIRS)

EXTRA_DIST = zbxwin32
