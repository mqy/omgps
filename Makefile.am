############ resources ######################

confdir = $(sysconfdir)/omgps/${VERSION}
dist_conf_DATA = data/etc/*.*

EXTRA_DIST = $(dist_conf_DATA)

desktopdir = $(datadir)/applications
dist_desktop_DATA = data/omgps.desktop

appicondir = $(datadir)/pixmaps/omgps
dist_appicon_DATA = data/omgps.png data/sky.png

############ source files ###################

# FIXME: detect version automatically
PY_VERSION = 2.6

# If not bitbake, set STAGING_DIR as LIBTOOL_SYSROOT_PATH for normal cross compiling.
OPIEDIR ?= /usr/local/openmoko/arm/arm-angstrom-linux-gnueabi/
PY_INC_DIR = $(OPIEDIR)/usr/include/python$(PY_VERSION)/

bin_PROGRAMS = omgps

common_CFLAGS = -Wall -Werror       \
  -I$(PY_INC_DIR)                   \
  -I$(top_srcdir)/src               \
  -I$(top_srcdir)/src/include       \
  -DCONF_DIR=\"$(confdir)\"         \
  -DICONDIR=\"$(appicondir)\"       \
  -D_GNU_SOURCE                     \
  @DEPENDENCIES_CFLAGS@

omgps_CFLAGS = $(common_CFLAGS) -g
omgps_LDADD = @DEPENDENCIES_LIBS@ -lpython$(PY_VERSION)

omgps_SOURCES =          \
  src/ctx_agps_online.c  \
  src/ctx_fixmap.c       \
  src/ctx_dl_tiles.c     \
  src/ctx_track_replay.c \
  src/cr_pixbuf.c        \
  src/customized.c       \
  src/dbus_intf.c        \
  src/gc_resource.c      \
  src/globals.c          \
  src/main.c             \ 
  src/map_repo.c         \
  src/map_meter.c        \
  src/mouse.c            \
  src/network.c          \
  src/poll.c             \
  src/poll_ui.c	         \
  src/py_ext.c	         \
  src/settings.c         \
  src/sound.c            \
  src/tab_gpscfg.c       \
  src/tab_menu.c         \
  src/tab_nav.c	         \
  src/tab_scratch.c      \
  src/tab_sound.c        \
  src/tab_tile.c         \
  src/tab_track.c        \
  src/tab_view.c         \
  src/tile_dl.c          \
  src/tile_cache.c       \
  src/ubx.c              \
  src/util.c             \
  src/uart.c             \
  src/wgs84.c            \
  src/xpm_image.c