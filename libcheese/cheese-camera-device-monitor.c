/*
 * Copyright © 2007,2008 Jaap Haitsma <jaap@haitsma.org>
 * Copyright © 2007-2009 daniel g. siegel <dgsiegel@gnome.org>
 * Copyright © 2008 Ryan Zeigler <zeiglerr@gmail.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifdef HAVE_CONFIG_H
  #include <cheese-config.h>
#endif

#include <glib-object.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <string.h>

#ifdef HAVE_UDEV
#define G_UDEV_API_IS_SUBJECT_TO_CHANGE 1
#include <gudev/gudev.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#if USE_SYS_VIDEOIO_H > 0
#include <sys/types.h>
#include <sys/videoio.h>
#elif defined(__sun)
#include <sys/types.h>
#include <sys/videodev2.h>
#endif /* USE_SYS_VIDEOIO_H */
#endif

#include "cheese-camera-device-monitor.h"

G_DEFINE_TYPE (CheeseCameraDeviceMonitor, cheese_camera_device_monitor, G_TYPE_OBJECT)

#define CHEESE_CAMERA_DEVICE_MONITOR_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), \
	  CHEESE_TYPE_CAMERA_DEVICE_MONITOR, CheeseCameraDeviceMonitorPrivate))

#define CHEESE_CAMERA_DEVICE_MONITOR_ERROR cheese_camera_device_monitor_error_quark ()

enum CheeseCameraDeviceMonitorError
{
  CHEESE_CAMERA_DEVICE_MONITOR_ERROR_UNKNOWN,
  CHEESE_CAMERA_DEVICE_MONITOR_ERROR_ELEMENT_NOT_FOUND
};

typedef struct
{
#ifdef HAVE_UDEV
  GUdevClient *client;
#else
  guint filler;
#endif /* HAVE_UDEV */
} CheeseCameraDeviceMonitorPrivate;

enum
{
  ADDED,
  REMOVED,
  LAST_SIGNAL
};

static guint monitor_signals[LAST_SIGNAL];

GQuark
cheese_camera_device_monitor_error_quark (void)
{
  return g_quark_from_static_string ("cheese-camera-error-quark");
}

#ifdef HAVE_UDEV
static void
cheese_camera_device_monitor_added (CheeseCameraDeviceMonitor *monitor,
				    GUdevDevice *udevice)
{
  const char             *device_path;
  const char             *gstreamer_src, *product_name;
  const char             *vendor;
  const char             *product;
  gint                    vendor_id     = 0;
  gint                    product_id    = 0;
  gint                    v4l_version   = 0;
  CheeseCameraDevice     *device;

  g_print ("Checking udev device '%s'\n", g_udev_device_get_property (udevice, "DEVPATH"));

  vendor = g_udev_device_get_property (udevice, "ID_VENDOR_ID");
  if (vendor != NULL)
    vendor_id = g_ascii_strtoll (vendor, NULL, 16);
  product = g_udev_device_get_property (udevice, "ID_MODEL_ID");
  if (product != NULL)
    product_id = g_ascii_strtoll (vendor, NULL, 16);
  if (vendor_id == 0 || product_id == 0) {
    g_warning ("error getting product or vendor id");
    return;
  }
  g_print ("Found device %04x:%04x, getting capabilities...\n", vendor_id, product_id);

  device_path = g_udev_device_get_device_file (udevice);
  if (device_path == NULL) {
    g_warning ("error getting V4L device");
    return;
  }

  /* vbi devices support capture capability too, but cannot be used,
   * so detect them by device name */
  if (strstr (device_path, "vbi"))
  {
    g_print ("Skipping vbi device: %s\n", device_path);
    return;
  }

  v4l_version = g_udev_device_get_property_as_int (udevice, "ID_V4L_VERSION");
  if (v4l_version == 2 || v4l_version == 1) {
    const char *caps;

    caps = g_udev_device_get_property (udevice, "ID_V4L_CAPABILITIES");
    if (caps == NULL || strstr (caps, ":capture:") == NULL)
    {
      g_print ("Device %s seems to not have the capture capability, (radio tuner?)\n"
	       "Removing it from device list.\n", device_path);
      return;
    }
    gstreamer_src = (v4l_version == 2) ? "v4l2src" : "v4lsrc";
    product_name = g_udev_device_get_property (udevice, "ID_V4L_PRODUCT");
  } else if (v4l_version == 0) {
    g_warning ("Fix your udev installation to include v4l_id, ignoring %s", device_path);
    return;
  } else {
    g_assert_not_reached ();
  }

  g_print ("\n");

  device = g_new0 (CheeseCameraDevice, 1);

  device->id                = g_strdup (g_udev_device_get_property (udevice, "DEVPATH"));
  device->video_device      = g_strdup (device_path);
  device->gstreamer_src     = g_strdup (gstreamer_src);
  device->product_name      = g_strdup (product_name);
  device->num_video_formats = 0;
  device->video_formats     = g_array_new (FALSE, FALSE, sizeof (CheeseVideoFormat));
  device->supported_resolutions =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  //FIXME This will leak a device, we should ref/unref it instead
  g_signal_emit (monitor, monitor_signals[ADDED], 0, device);
}

static void
cheese_camera_device_monitor_removed (CheeseCameraDeviceMonitor *monitor,
				      GUdevDevice *udevice)
{
  g_signal_emit (monitor, monitor_signals[REMOVED], 0,
		 g_udev_device_get_property (udevice, "DEVPATH"));
}

static void
cheese_camera_device_monitor_uevent_cb (GUdevClient  *client,
					const gchar  *action,
					GUdevDevice  *udevice,
					CheeseCameraDeviceMonitor *monitor)
{
  if (g_str_equal (action, "remove"))
    cheese_camera_device_monitor_removed (monitor, udevice);
  else if (g_str_equal (action, "add"))
    cheese_camera_device_monitor_added (monitor, udevice);
}

void
cheese_camera_device_monitor_coldplug (CheeseCameraDeviceMonitor *monitor)
{
  CheeseCameraDeviceMonitorPrivate *priv = CHEESE_CAMERA_DEVICE_MONITOR_GET_PRIVATE (monitor);
  GList *devices, *l;

  g_print ("Probing devices with udev...\n");

  if (priv->client == NULL)
    return;

  devices = g_udev_client_query_by_subsystem (priv->client, "video4linux");

  /* Initialize camera structures */
  for (l = devices; l != NULL; l = l->next) {
    cheese_camera_device_monitor_added (monitor, l->data);
    g_object_unref (l->data);
  }
  g_list_free (devices);
}
#else /* HAVE_UDEV */
void
cheese_camera_device_monitor_coldplug (CheeseCameraDeviceMonitor *monitor)
{
#if 0
  CheeseCameraDeviceMonitorPrivate *priv = CHEESE_CAMERA_DEVICE_MONITOR_GET_PRIVATE (monitor);
  struct v4l2_capability  v2cap;
  struct video_capability v1cap;
  int fd, ok;

  if ((fd = open (device_path, O_RDONLY | O_NONBLOCK)) < 0)
  {
    g_warning ("Failed to open %s: %s", device_path, strerror (errno));
    return;
  }
  ok = ioctl (fd, VIDIOC_QUERYCAP, &v2cap);
  if (ok < 0)
  {
    ok = ioctl (fd, VIDIOCGCAP, &v1cap);
    if (ok < 0)
    {
      g_warning ("Error while probing v4l capabilities for %s: %s",
		 device_path, strerror (errno));
      close (fd);
      return;
    }
    g_print ("Detected v4l device: %s\n", v1cap.name);
    g_print ("Device type: %d\n", v1cap.type);
    gstreamer_src = "v4lsrc";
    product_name  = v1cap.name;
  }
  else
  {
    guint cap = v2cap.capabilities;
    g_print ("Detected v4l2 device: %s\n", v2cap.card);
    g_print ("Driver: %s, version: %d\n", v2cap.driver, v2cap.version);

    /* g_print ("Bus info: %s\n", v2cap.bus_info); */ /* Doesn't seem anything useful */
    g_print ("Capabilities: 0x%08X\n", v2cap.capabilities);
    if (!(cap & V4L2_CAP_VIDEO_CAPTURE))
    {
      g_print ("Device %s seems to not have the capture capability, (radio tuner?)\n"
	       "Removing it from device list.\n", device_path);
      close (fd);
      return;
    }
    gstreamer_src = "v4l2src";
    product_name  = (char *) v2cap.card;
  }
  close (fd);

  GList *devices, *l;

  g_print ("Probing devices with udev...\n");

  if (priv->client == NULL)
    return;

  devices = g_udev_client_query_by_subsystem (priv->client, "video4linux");

  /* Initialize camera structures */
  for (l = devices; l != NULL; l = l->next) {
    cheese_camera_device_monitor_added (monitor, l->data);
    g_object_unref (l->data);
  }
  g_list_free (devices);
#endif
}
#endif /* HAVE_UDEV */

static void
cheese_camera_device_monitor_finalize (GObject *object)
{
#ifdef HAVE_UDEV
  CheeseCameraDeviceMonitor *monitor;

  monitor = CHEESE_CAMERA_DEVICE_MONITOR (object);
  CheeseCameraDeviceMonitorPrivate *priv = CHEESE_CAMERA_DEVICE_MONITOR_GET_PRIVATE (monitor);

  if (priv->client != NULL) {
    g_object_unref (priv->client);
    priv->client = NULL;
  }
#endif /* HAVE_UDEV */
  G_OBJECT_CLASS (cheese_camera_device_monitor_parent_class)->finalize (object);
}

static void
cheese_camera_device_monitor_class_init (CheeseCameraDeviceMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize     = cheese_camera_device_monitor_finalize;

  monitor_signals[ADDED] = g_signal_new ("added", G_OBJECT_CLASS_TYPE (klass),
					 G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
					 G_STRUCT_OFFSET (CheeseCameraDeviceMonitorClass, added),
					 NULL, NULL,
					 g_cclosure_marshal_VOID__POINTER,
					 G_TYPE_NONE, 1, G_TYPE_POINTER);

  monitor_signals[REMOVED] = g_signal_new ("removed", G_OBJECT_CLASS_TYPE (klass),
					   G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
					   G_STRUCT_OFFSET (CheeseCameraDeviceMonitorClass, removed),
					   NULL, NULL,
					   g_cclosure_marshal_VOID__STRING,
					   G_TYPE_NONE, 1, G_TYPE_STRING);

  g_type_class_add_private (klass, sizeof (CheeseCameraDeviceMonitorPrivate));
}

static void
cheese_camera_device_monitor_init (CheeseCameraDeviceMonitor *monitor)
{
#ifdef HAVE_UDEV
  CheeseCameraDeviceMonitorPrivate *priv = CHEESE_CAMERA_DEVICE_MONITOR_GET_PRIVATE (monitor);
  const gchar* const subsystems[] = { "video4linux", NULL };

  priv->client = g_udev_client_new (subsystems);
  g_signal_connect (G_OBJECT (priv->client), "uevent",
		    G_CALLBACK (cheese_camera_device_monitor_uevent_cb), monitor);
#endif /* HAVE_UDEV */
}

CheeseCameraDeviceMonitor *
cheese_camera_device_monitor_new (void)
{
  return g_object_new (CHEESE_TYPE_CAMERA_DEVICE_MONITOR, NULL);
}

/*
 * vim: sw=2 ts=8 cindent noai bs=2
 */
