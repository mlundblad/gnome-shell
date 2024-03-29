/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * st-texture-cache.h: Object for loading and caching images as textures
 *
 * Copyright 2009, 2010 Red Hat, Inc.
 * Copyright 2010, Maxim Ermilov
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "st-texture-cache.h"
#include "st-private.h"
#include <gtk/gtk.h>
#include <string.h>
#include <glib.h>

#define CACHE_PREFIX_ICON "icon:"
#define CACHE_PREFIX_URI "uri:"
#define CACHE_PREFIX_URI_FOR_CAIRO "uri-for-cairo:"
#define CACHE_PREFIX_RAW_CHECKSUM "raw-checksum:"
#define CACHE_PREFIX_COMPRESSED_CHECKSUM "compressed-checksum:"

struct _StTextureCachePrivate
{
  GtkIconTheme *icon_theme;

  /* Things that were loaded with a cache policy != NONE */
  GHashTable *keyed_cache; /* char * -> CoglTexture* */

  /* Presently this is used to de-duplicate requests for GIcons and async URIs. */
  GHashTable *outstanding_requests; /* char * -> AsyncTextureLoadData * */

  /* File monitors to evict cache data on changes */
  GHashTable *file_monitors; /* char * -> GFileMonitor * */
};

static void st_texture_cache_dispose (GObject *object);
static void st_texture_cache_finalize (GObject *object);

enum
{
  ICON_THEME_CHANGED,
  TEXTURE_FILE_CHANGED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };
G_DEFINE_TYPE(StTextureCache, st_texture_cache, G_TYPE_OBJECT);

/* We want to preserve the aspect ratio by default, also the default
 * material for an empty texture is full opacity white, which we
 * definitely don't want.  Skip that by setting 0 opacity.
 */
static ClutterTexture *
create_default_texture (void)
{
  ClutterTexture * texture = CLUTTER_TEXTURE (clutter_texture_new ());
  g_object_set (texture, "keep-aspect-ratio", TRUE, "opacity", 0, NULL);
  return texture;
}

/* Reverse the opacity we added while loading */
static void
set_texture_cogl_texture (ClutterTexture *clutter_texture, CoglHandle cogl_texture)
{
  clutter_texture_set_cogl_texture (clutter_texture, cogl_texture);
  g_object_set (clutter_texture, "opacity", 255, NULL);
}

static void
st_texture_cache_class_init (StTextureCacheClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *)klass;

  gobject_class->dispose = st_texture_cache_dispose;
  gobject_class->finalize = st_texture_cache_finalize;

  signals[ICON_THEME_CHANGED] =
    g_signal_new ("icon-theme-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, /* no default handler slot */
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[TEXTURE_FILE_CHANGED] =
    g_signal_new ("texture-file-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, /* no default handler slot */
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_STRING);
}

/* Evicts all cached textures for named icons */
static void
st_texture_cache_evict_icons (StTextureCache *cache)
{
  GHashTableIter iter;
  gpointer key;
  gpointer value;

  g_hash_table_iter_init (&iter, cache->priv->keyed_cache);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *cache_key = key;

      /* This is too conservative - it takes out all cached textures
       * for GIcons even when they aren't named icons, but it's not
       * worth the complexity of parsing the key and calling
       * g_icon_new_for_string(); icon theme changes aren't normal */
      if (g_str_has_prefix (cache_key, CACHE_PREFIX_ICON))
        g_hash_table_iter_remove (&iter);
    }
}

static void
on_icon_theme_changed (GtkIconTheme   *icon_theme,
                       StTextureCache *cache)
{
  st_texture_cache_evict_icons (cache);
  g_signal_emit (cache, signals[ICON_THEME_CHANGED], 0);
}

static void
st_texture_cache_init (StTextureCache *self)
{
  self->priv = g_new0 (StTextureCachePrivate, 1);

  self->priv->icon_theme = gtk_icon_theme_get_default ();
  g_signal_connect (self->priv->icon_theme, "changed",
                    G_CALLBACK (on_icon_theme_changed), self);

  self->priv->keyed_cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                   g_free, cogl_handle_unref);
  self->priv->outstanding_requests = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                            g_free, NULL);
  self->priv->file_monitors = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                     g_object_unref, g_object_unref);

}

static void
st_texture_cache_dispose (GObject *object)
{
  StTextureCache *self = (StTextureCache*)object;

  if (self->priv->icon_theme)
    {
      g_signal_handlers_disconnect_by_func (self->priv->icon_theme,
                                            (gpointer) on_icon_theme_changed,
                                            self);
      self->priv->icon_theme = NULL;
    }

  g_clear_pointer (&self->priv->keyed_cache, g_hash_table_destroy);
  g_clear_pointer (&self->priv->outstanding_requests, g_hash_table_destroy);
  g_clear_pointer (&self->priv->file_monitors, g_hash_table_destroy);

  G_OBJECT_CLASS (st_texture_cache_parent_class)->dispose (object);
}

static void
st_texture_cache_finalize (GObject *object)
{
  G_OBJECT_CLASS (st_texture_cache_parent_class)->finalize (object);
}

static gboolean
compute_pixbuf_scale (gint      width,
                      gint      height,
                      gint      available_width,
                      gint      available_height,
                      gint     *new_width,
                      gint     *new_height)
{
  int scaled_width, scaled_height;

  if (width == 0 || height == 0)
    return FALSE;

  if (available_width >= 0 && available_height >= 0)
    {
      /* This should keep the aspect ratio of the image intact, because if
       * available_width < (available_height * width) / height
       * then
       * (available_width * height) / width < available_height
       * So we are guaranteed to either scale the image to have an available_width
       * for width and height scaled accordingly OR have the available_height
       * for height and width scaled accordingly, whichever scaling results
       * in the image that can fit both available dimensions.
       */
      scaled_width = MIN (available_width, (available_height * width) / height);
      scaled_height = MIN (available_height, (available_width * height) / width);
    }
  else if (available_width >= 0)
    {
      scaled_width = available_width;
      scaled_height = (available_width * height) / width;
    }
  else if (available_height >= 0)
    {
      scaled_width = (available_height * width) / height;
      scaled_height = available_height;
    }
  else
    {
      scaled_width = scaled_height = 0;
    }

  /* Scale the image only if that will not increase its original dimensions. */
  if (scaled_width > 0 && scaled_height > 0 && scaled_width < width && scaled_height < height)
    {
      *new_width = scaled_width;
      *new_height = scaled_height;
      return TRUE;
    }
  return FALSE;
}

static void
rgba_from_clutter (GdkRGBA      *rgba,
                   ClutterColor *color)
{
  rgba->red = color->red / 255.;
  rgba->green = color->green / 255.;
  rgba->blue = color->blue / 255.;
  rgba->alpha = color->alpha / 255.;
}

static GdkPixbuf *
impl_load_pixbuf_gicon (GtkIconInfo  *info,
                        int           size,
                        StIconColors *colors,
                        GError      **error)
{
  int scaled_width, scaled_height;
  GdkPixbuf *pixbuf;
  int width, height;

  if (colors)
    {
      GdkRGBA foreground_color;
      GdkRGBA success_color;
      GdkRGBA warning_color;
      GdkRGBA error_color;

      rgba_from_clutter (&foreground_color, &colors->foreground);
      rgba_from_clutter (&success_color, &colors->success);
      rgba_from_clutter (&warning_color, &colors->warning);
      rgba_from_clutter (&error_color, &colors->error);

      pixbuf = gtk_icon_info_load_symbolic (info,
                                            &foreground_color, &success_color,
                                            &warning_color, &error_color,
                                            NULL, error);
    }
  else
    {
      pixbuf = gtk_icon_info_load_icon (info, error);
    }

  if (!pixbuf)
    return NULL;

  width = gdk_pixbuf_get_width (pixbuf);
  height = gdk_pixbuf_get_height (pixbuf);

  if (compute_pixbuf_scale (width,
                            height,
                            size, size,
                            &scaled_width, &scaled_height))
    {
      GdkPixbuf *scaled = gdk_pixbuf_scale_simple (pixbuf, width, height, GDK_INTERP_BILINEAR);
      g_object_unref (pixbuf);
      pixbuf = scaled;
    }
  return pixbuf;
}

/* A private structure for keeping width and height. */
typedef struct {
  int width;
  int height;
} Dimensions;

/* This struct corresponds to a request for an texture.
 * It's creasted when something needs a new texture,
 * and destroyed when the texture data is loaded. */
typedef struct {
  StTextureCache *cache;
  StTextureCachePolicy policy;
  char *key;

  gboolean enforced_square;

  guint width;
  guint height;
  GSList *textures;

  GtkIconInfo *icon_info;
  StIconColors *colors;
  char *uri;
} AsyncTextureLoadData;

static void
texture_load_data_destroy (gpointer p)
{
  AsyncTextureLoadData *data = p;

  if (data->icon_info)
    {
      gtk_icon_info_free (data->icon_info);
      if (data->colors)
        st_icon_colors_unref (data->colors);
    }
  else if (data->uri)
    g_free (data->uri);

  if (data->key)
    g_free (data->key);

  if (data->textures)
    g_slist_free_full (data->textures, (GDestroyNotify) g_object_unref);
}

/**
 * on_image_size_prepared:
 * @pixbuf_loader: #GdkPixbufLoader loading the image
 * @width: the original width of the image
 * @height: the original height of the image
 * @data: pointer to the #Dimensions sructure containing available width and height for the image,
 *        available width or height can be -1 if the dimension is not limited
 *
 * Private function.
 *
 * Sets the size of the image being loaded to fit the available width and height dimensions,
 * but never scales up the image beyond its actual size.
 * Intended to be used as a callback for #GdkPixbufLoader "size-prepared" signal.
 */
static void
on_image_size_prepared (GdkPixbufLoader *pixbuf_loader,
                        gint             width,
                        gint             height,
                        gpointer         data)
{
  Dimensions *available_dimensions = data;
  int available_width = available_dimensions->width;
  int available_height = available_dimensions->height;
  int scaled_width;
  int scaled_height;

  if (compute_pixbuf_scale (width, height, available_width, available_height,
                            &scaled_width, &scaled_height))
    gdk_pixbuf_loader_set_size (pixbuf_loader, scaled_width, scaled_height);
}

static GdkPixbuf *
impl_load_pixbuf_data (const guchar   *data,
                       gsize           size,
                       int             available_width,
                       int             available_height,
                       GError        **error)
{
  GdkPixbufLoader *pixbuf_loader = NULL;
  GdkPixbuf *rotated_pixbuf = NULL;
  GdkPixbuf *pixbuf;
  gboolean success;
  Dimensions available_dimensions;
  int width_before_rotation, width_after_rotation;

  pixbuf_loader = gdk_pixbuf_loader_new ();

  available_dimensions.width = available_width;
  available_dimensions.height = available_height;
  g_signal_connect (pixbuf_loader, "size-prepared",
                    G_CALLBACK (on_image_size_prepared), &available_dimensions);

  success = gdk_pixbuf_loader_write (pixbuf_loader, data, size, error);
  if (!success)
    goto out;
  success = gdk_pixbuf_loader_close (pixbuf_loader, error);
  if (!success)
    goto out;

  pixbuf = gdk_pixbuf_loader_get_pixbuf (pixbuf_loader);

  width_before_rotation = gdk_pixbuf_get_width (pixbuf);

  rotated_pixbuf = gdk_pixbuf_apply_embedded_orientation (pixbuf);
  width_after_rotation = gdk_pixbuf_get_width (rotated_pixbuf);

  /* There is currently no way to tell if the pixbuf will need to be rotated before it is loaded,
   * so we only check that once it is loaded, and reload it again if it needs to be rotated in order
   * to use the available width and height correctly.
   * See http://bugzilla.gnome.org/show_bug.cgi?id=579003
   */
  if (width_before_rotation != width_after_rotation)
    {
      g_object_unref (pixbuf_loader);
      g_object_unref (rotated_pixbuf);
      rotated_pixbuf = NULL;

      pixbuf_loader = gdk_pixbuf_loader_new ();

      /* We know that the image will later be rotated, so we reverse the available dimensions. */
      available_dimensions.width = available_height;
      available_dimensions.height = available_width;
      g_signal_connect (pixbuf_loader, "size-prepared",
                        G_CALLBACK (on_image_size_prepared), &available_dimensions);

      success = gdk_pixbuf_loader_write (pixbuf_loader, data, size, error);
      if (!success)
        goto out;

      success = gdk_pixbuf_loader_close (pixbuf_loader, error);
      if (!success)
        goto out;

      pixbuf = gdk_pixbuf_loader_get_pixbuf (pixbuf_loader);

      rotated_pixbuf = gdk_pixbuf_apply_embedded_orientation (pixbuf);
    }

out:
  if (pixbuf_loader)
    g_object_unref (pixbuf_loader);
  return rotated_pixbuf;
}

static GdkPixbuf*
decode_image (const char *val)
{
  int i;
  GError *error = NULL;
  GdkPixbuf *res = NULL;
  struct {
    const char *prefix;
    const char *mime_type;
  } formats[] = {
    { "data:image/x-icon;base64,", "image/x-icon" },
    { "data:image/png;base64,", "image/png" }
  };

  g_return_val_if_fail (val, NULL);

  for (i = 0; i < G_N_ELEMENTS (formats); i++)
    {
      if (g_str_has_prefix (val, formats[i].prefix))
        {
          gsize len;
          guchar *data = NULL;
          char *unescaped;

          unescaped = g_uri_unescape_string (val + strlen (formats[i].prefix), NULL);
          if (unescaped)
            {
              data = g_base64_decode (unescaped, &len);
              g_free (unescaped);
            }

          if (data)
            {
              GdkPixbufLoader *loader;

              loader = gdk_pixbuf_loader_new_with_mime_type (formats[i].mime_type, &error);
              if (loader &&
                  gdk_pixbuf_loader_write (loader, data, len, &error) &&
                  gdk_pixbuf_loader_close (loader, &error))
                {
                  res = gdk_pixbuf_loader_get_pixbuf (loader);
                  g_object_ref (res);
                }
              g_object_unref (loader);
              g_free (data);
            }
        }
    }
  if (!res)
    {
      if (error)
        {
          g_warning ("%s\n", error->message);
          g_error_free (error);
        }
      else
        g_warning ("incorrect data uri");
    }
  return res;
}

static GdkPixbuf *
impl_load_pixbuf_file (const char     *uri,
                       int             available_width,
                       int             available_height,
                       GError        **error)
{
  GdkPixbuf *pixbuf = NULL;
  GFile *file;
  char *contents = NULL;
  gsize size;

  if (g_str_has_prefix (uri, "data:"))
    return decode_image (uri);

  file = g_file_new_for_uri (uri);
  if (g_file_load_contents (file, NULL, &contents, &size, NULL, error))
    {
      pixbuf = impl_load_pixbuf_data ((const guchar *) contents, size,
                                      available_width, available_height,
                                      error);
    }

  g_object_unref (file);
  g_free (contents);

  return pixbuf;
}

static void
load_pixbuf_thread (GSimpleAsyncResult *result,
                    GObject *object,
                    GCancellable *cancellable)
{
  GdkPixbuf *pixbuf;
  AsyncTextureLoadData *data;
  GError *error = NULL;

  data = g_async_result_get_user_data (G_ASYNC_RESULT (result));
  g_assert (data != NULL);

  if (data->uri)
    pixbuf = impl_load_pixbuf_file (data->uri, data->width, data->height, &error);
  else if (data->icon_info)
    pixbuf = impl_load_pixbuf_gicon (data->icon_info, data->width, data->colors, &error);
  else
    g_assert_not_reached ();

  if (error != NULL)
    {
      g_simple_async_result_set_from_error (result, error);
      return;
    }

  if (pixbuf)
    g_simple_async_result_set_op_res_gpointer (result, g_object_ref (pixbuf),
                                               g_object_unref);
}

static GdkPixbuf *
load_pixbuf_async_finish (StTextureCache *cache, GAsyncResult *result, GError **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  if (g_simple_async_result_propagate_error (simple, error))
    return NULL;
  return g_simple_async_result_get_op_res_gpointer (simple);
}

static CoglHandle
data_to_cogl_handle (const guchar *data,
                     gboolean      has_alpha,
                     int           width,
                     int           height,
                     int           rowstride,
                     gboolean      add_padding)
{
  CoglHandle texture, offscreen;
  CoglColor clear_color;
  guint size;

  size = MAX (width, height);

  if (!add_padding || width == height)
    return cogl_texture_new_from_data (width,
                                       height,
                                       COGL_TEXTURE_NONE,
                                       has_alpha ? COGL_PIXEL_FORMAT_RGBA_8888 : COGL_PIXEL_FORMAT_RGB_888,
                                       COGL_PIXEL_FORMAT_ANY,
                                       rowstride,
                                       data);

  texture = cogl_texture_new_with_size (size, size,
                                        COGL_TEXTURE_NO_SLICING,
                                        COGL_PIXEL_FORMAT_ANY);

  offscreen = cogl_offscreen_new_to_texture (texture);
  cogl_color_set_from_4ub (&clear_color, 0, 0, 0, 0);
  cogl_push_framebuffer (offscreen);
  cogl_clear (&clear_color, COGL_BUFFER_BIT_COLOR);
  cogl_pop_framebuffer ();
  cogl_handle_unref (offscreen);

  cogl_texture_set_region (texture,
                           0, 0,
                           (size - width) / 2, (size - height) / 2,
                           width, height,
                           width, height,
                           has_alpha ? COGL_PIXEL_FORMAT_RGBA_8888 : COGL_PIXEL_FORMAT_RGB_888,
                           rowstride,
                           data);
  return texture;
}

static CoglHandle
pixbuf_to_cogl_handle (GdkPixbuf *pixbuf,
                       gboolean   add_padding)
{
  return data_to_cogl_handle (gdk_pixbuf_get_pixels (pixbuf),
                              gdk_pixbuf_get_has_alpha (pixbuf),
                              gdk_pixbuf_get_width (pixbuf),
                              gdk_pixbuf_get_height (pixbuf),
                              gdk_pixbuf_get_rowstride (pixbuf),
                              add_padding);
}

static cairo_surface_t *
pixbuf_to_cairo_surface (GdkPixbuf *pixbuf)
{
  cairo_surface_t *dummy_surface;
  cairo_pattern_t *pattern;
  cairo_surface_t *surface;
  cairo_t *cr;

  dummy_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 1, 1);

  cr = cairo_create (dummy_surface);
  gdk_cairo_set_source_pixbuf (cr, pixbuf, 0, 0);
  pattern = cairo_get_source (cr);
  cairo_pattern_get_surface (pattern, &surface);
  cairo_surface_reference (surface);
  cairo_destroy (cr);
  cairo_surface_destroy (dummy_surface);

  return surface;
}

static void
on_pixbuf_loaded (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  GSList *iter;
  StTextureCache *cache;
  AsyncTextureLoadData *data;
  GdkPixbuf *pixbuf;
  GError *error = NULL;
  CoglHandle texdata = NULL;

  data = user_data;
  cache = ST_TEXTURE_CACHE (source);

  g_hash_table_remove (cache->priv->outstanding_requests, data->key);

  pixbuf = load_pixbuf_async_finish (cache, result, &error);
  if (pixbuf == NULL)
    goto out;

  texdata = pixbuf_to_cogl_handle (pixbuf, data->enforced_square);

  g_object_unref (pixbuf);

  if (data->policy != ST_TEXTURE_CACHE_POLICY_NONE)
    {
      gpointer orig_key, value;

      if (!g_hash_table_lookup_extended (cache->priv->keyed_cache, data->key,
                                         &orig_key, &value))
        {
          cogl_handle_ref (texdata);
          g_hash_table_insert (cache->priv->keyed_cache, g_strdup (data->key),
                               texdata);
        }
    }

  for (iter = data->textures; iter; iter = iter->next)
    {
      ClutterTexture *texture = iter->data;
      set_texture_cogl_texture (texture, texdata);
    }

out:
  if (texdata)
    cogl_handle_unref (texdata);

  texture_load_data_destroy (data);
  g_free (data);

  g_clear_error (&error);
}

static void
load_texture_async (StTextureCache       *cache,
                    AsyncTextureLoadData *data)
{
  GSimpleAsyncResult *result;
  result = g_simple_async_result_new (G_OBJECT (cache), on_pixbuf_loaded, data, load_texture_async);
  g_simple_async_result_run_in_thread (result, load_pixbuf_thread, G_PRIORITY_DEFAULT, NULL);
  g_object_unref (result);
}

typedef struct {
  StTextureCache *cache;
  ClutterTexture *texture;
  GObject *source;
  guint notify_signal_id;
  gboolean weakref_active;
} StTextureCachePropertyBind;

static void
st_texture_cache_reset_texture (StTextureCachePropertyBind *bind,
                                const char                 *propname)
{
  GdkPixbuf *pixbuf;
  CoglHandle texdata;

  g_object_get (bind->source, propname, &pixbuf, NULL);

  g_return_if_fail (pixbuf == NULL || GDK_IS_PIXBUF (pixbuf));

  if (pixbuf != NULL)
    {
      texdata = pixbuf_to_cogl_handle (pixbuf, FALSE);
      g_object_unref (pixbuf);

      clutter_texture_set_cogl_texture (bind->texture, texdata);
      cogl_handle_unref (texdata);

      clutter_actor_set_opacity (CLUTTER_ACTOR (bind->texture), 255);
    }
  else
    clutter_actor_set_opacity (CLUTTER_ACTOR (bind->texture), 0);
}

static void
st_texture_cache_on_pixbuf_notify (GObject           *object,
                                   GParamSpec        *paramspec,
                                   gpointer           data)
{
  StTextureCachePropertyBind *bind = data;
  st_texture_cache_reset_texture (bind, paramspec->name);
}

static void
st_texture_cache_bind_weak_notify (gpointer     data,
                                   GObject     *source_location)
{
  StTextureCachePropertyBind *bind = data;
  bind->weakref_active = FALSE;
  g_signal_handler_disconnect (bind->source, bind->notify_signal_id);
}

static void
st_texture_cache_free_bind (gpointer data)
{
  StTextureCachePropertyBind *bind = data;
  if (bind->weakref_active)
    g_object_weak_unref (G_OBJECT(bind->texture), st_texture_cache_bind_weak_notify, bind);
  g_free (bind);
}

/**
 * st_texture_cache_bind_pixbuf_property:
 * @cache:
 * @object: A #GObject with a property @property_name of type #GdkPixbuf
 * @property_name: Name of a property
 *
 * Create a #ClutterTexture which tracks the #GdkPixbuf value of a GObject property
 * named by @property_name.  Unlike other methods in StTextureCache, the underlying
 * CoglHandle is not shared by default with other invocations to this method.
 *
 * If the source object is destroyed, the texture will continue to show the last
 * value of the property.
 *
 * Return value: (transfer none): A new #ClutterActor
 */
ClutterActor *
st_texture_cache_bind_pixbuf_property (StTextureCache    *cache,
                                       GObject           *object,
                                       const char        *property_name)
{
  ClutterTexture *texture;
  gchar *notify_key;
  StTextureCachePropertyBind *bind;

  texture = CLUTTER_TEXTURE (clutter_texture_new ());

  bind = g_new0 (StTextureCachePropertyBind, 1);
  bind->cache = cache;
  bind->texture = texture;
  bind->source = object;
  g_object_weak_ref (G_OBJECT (texture), st_texture_cache_bind_weak_notify, bind);
  bind->weakref_active = TRUE;

  st_texture_cache_reset_texture (bind, property_name);

  notify_key = g_strdup_printf ("notify::%s", property_name);
  bind->notify_signal_id = g_signal_connect_data (object, notify_key, G_CALLBACK(st_texture_cache_on_pixbuf_notify),
                                                  bind, (GClosureNotify)st_texture_cache_free_bind, 0);
  g_free (notify_key);

  return CLUTTER_ACTOR(texture);
}

/**
 * st_texture_cache_load: (skip)
 * @cache: A #StTextureCache
 * @key: Arbitrary string used to refer to item
 * @policy: Caching policy
 * @load: Function to create the texture, if not already cached
 * @data: User data passed to @load
 * @error: A #GError
 *
 * Load an arbitrary texture, caching it.  The string chosen for @key
 * should be of the form "type-prefix:type-uuid".  For example,
 * "url:file:///usr/share/icons/hicolor/48x48/apps/firefox.png", or
 * "stock-icon:gtk-ok".
 *
 * Returns: (transfer full): A newly-referenced handle to the texture
 */
CoglHandle
st_texture_cache_load (StTextureCache       *cache,
                       const char           *key,
                       StTextureCachePolicy  policy,
                       StTextureCacheLoader  load,
                       void                 *data,
                       GError              **error)
{
  CoglHandle texture;

  texture = g_hash_table_lookup (cache->priv->keyed_cache, key);
  if (!texture)
    {
      texture = load (cache, key, data, error);
      if (texture)
        g_hash_table_insert (cache->priv->keyed_cache, g_strdup (key), texture);
      else
        return COGL_INVALID_HANDLE;
    }
  cogl_handle_ref (texture);
  return texture;
}

/**
 * ensure_request:
 * @cache:
 * @key: A cache key
 * @policy: Cache policy
 * @request: (out): If no request is outstanding, one will be created and returned here
 * @texture: A texture to be added to the request
 *
 * Check for any outstanding load for the data represented by @key.  If there
 * is already a request pending, append it to that request to avoid loading
 * the data multiple times.
 *
 * Returns: %TRUE iff there is already a request pending
 */
static gboolean
ensure_request (StTextureCache        *cache,
                const char            *key,
                StTextureCachePolicy   policy,
                AsyncTextureLoadData **request,
                ClutterActor          *texture)
{
  CoglHandle texdata;
  AsyncTextureLoadData *pending;
  gboolean had_pending;

  texdata = g_hash_table_lookup (cache->priv->keyed_cache, key);

  if (texdata != NULL)
    {
      /* We had this cached already, just set the texture and we're done. */
      set_texture_cogl_texture (CLUTTER_TEXTURE (texture), texdata);
      return TRUE;
    }

  pending = g_hash_table_lookup (cache->priv->outstanding_requests, key);
  had_pending = pending != NULL;

  if (pending == NULL)
    {
      /* Not cached and no pending request, create it */
      *request = g_new0 (AsyncTextureLoadData, 1);
      if (policy != ST_TEXTURE_CACHE_POLICY_NONE)
        g_hash_table_insert (cache->priv->outstanding_requests, g_strdup (key), *request);
    }
  else
   *request = pending;

  /* Regardless of whether there was a pending request, prepend our texture here. */
  (*request)->textures = g_slist_prepend ((*request)->textures, g_object_ref (texture));

  return had_pending;
}

static ClutterActor *
load_gicon_with_colors (StTextureCache    *cache,
                        GIcon             *icon,
                        gint               size,
                        StIconColors      *colors)
{
  AsyncTextureLoadData *request;
  ClutterActor *texture;
  char *gicon_string;
  char *key;
  GtkIconTheme *theme;
  GtkIconInfo *info;
  StTextureCachePolicy policy;

  /* Do theme lookups in the main thread to avoid thread-unsafety */
  theme = cache->priv->icon_theme;

  info = gtk_icon_theme_lookup_by_gicon (theme, icon, size, GTK_ICON_LOOKUP_USE_BUILTIN);
  if (info == NULL)
    return NULL;

  gicon_string = g_icon_to_string (icon);
  /* A return value of NULL indicates that the icon can not be serialized,
   * so don't have a unique identifier for it as a cache key, and thus can't
   * be cached. If it is cachable, we hardcode a policy of FOREVER here for
   * now; we should actually blow this away on icon theme changes probably */
  policy = gicon_string != NULL ? ST_TEXTURE_CACHE_POLICY_FOREVER
                                : ST_TEXTURE_CACHE_POLICY_NONE;
  if (colors)
    {
      /* This raises some doubts about the practice of using string keys */
      key = g_strdup_printf (CACHE_PREFIX_ICON "%s,size=%d,colors=%2x%2x%2x%2x,%2x%2x%2x%2x,%2x%2x%2x%2x,%2x%2x%2x%2x",
                             gicon_string, size,
                             colors->foreground.red, colors->foreground.blue, colors->foreground.green, colors->foreground.alpha,
                             colors->warning.red, colors->warning.blue, colors->warning.green, colors->warning.alpha,
                             colors->error.red, colors->error.blue, colors->error.green, colors->error.alpha,
                             colors->success.red, colors->success.blue, colors->success.green, colors->success.alpha);
    }
  else
    {
      key = g_strdup_printf (CACHE_PREFIX_ICON "%s,size=%d",
                             gicon_string, size);
    }
  g_free (gicon_string);

  texture = (ClutterActor *) create_default_texture ();
  clutter_actor_set_size (texture, size, size);

  if (ensure_request (cache, key, policy, &request, texture))
    {
      /* If there's an outstanding request, we've just added ourselves to it */
      gtk_icon_info_free (info);
      g_free (key);
    }
  else
    {
      /* Else, make a new request */

      request->cache = cache;
      /* Transfer ownership of key */
      request->key = key;
      request->policy = policy;
      request->colors = colors ? st_icon_colors_ref (colors) : NULL;
      request->icon_info = info;
      request->width = request->height = size;
      request->enforced_square = TRUE;

      load_texture_async (cache, request);
    }

  return CLUTTER_ACTOR (texture);
}

/**
 * st_texture_cache_load_gicon:
 * @cache: The texture cache instance
 * @theme_node: (allow-none): The #StThemeNode to use for colors, or NULL
 *                            if the icon must not be recolored
 * @icon: the #GIcon to load
 * @size: Size of themed
 *
 * This method returns a new #ClutterActor for a given #GIcon. If the
 * icon isn't loaded already, the texture will be filled
 * asynchronously.
 *
 * Return Value: (transfer none): A new #ClutterActor for the icon, or %NULL if not found
 */
ClutterActor *
st_texture_cache_load_gicon (StTextureCache    *cache,
                             StThemeNode       *theme_node,
                             GIcon             *icon,
                             gint               size)
{
  return load_gicon_with_colors (cache, icon, size, theme_node ? st_theme_node_get_icon_colors (theme_node) : NULL);
}

static ClutterActor *
load_from_pixbuf (GdkPixbuf *pixbuf)
{
  ClutterTexture *texture;
  CoglHandle texdata;
  int width = gdk_pixbuf_get_width (pixbuf);
  int height = gdk_pixbuf_get_height (pixbuf);

  texture = create_default_texture ();

  clutter_actor_set_size (CLUTTER_ACTOR (texture), width, height);

  texdata = pixbuf_to_cogl_handle (pixbuf, FALSE);

  set_texture_cogl_texture (texture, texdata);

  cogl_handle_unref (texdata);
  return CLUTTER_ACTOR (texture);
}

static void
file_changed_cb (GFileMonitor      *monitor,
                 GFile             *file,
                 GFile             *other,
                 GFileMonitorEvent  event_type,
                 gpointer           user_data)
{
  StTextureCache *cache = user_data;
  char *uri, *key;

  if (event_type != G_FILE_MONITOR_EVENT_CHANGED)
    return;

  uri = g_file_get_uri (file);

  key = g_strconcat (CACHE_PREFIX_URI, uri, NULL);
  g_hash_table_remove (cache->priv->keyed_cache, key);
  g_free (key);

  key = g_strconcat (CACHE_PREFIX_URI_FOR_CAIRO, uri, NULL);
  g_hash_table_remove (cache->priv->keyed_cache, key);
  g_free (key);

  g_signal_emit (cache, signals[TEXTURE_FILE_CHANGED], 0, uri);

  g_free (uri);
}

static void
ensure_monitor_for_uri (StTextureCache *cache,
                        const gchar    *uri)
{
  StTextureCachePrivate *priv = cache->priv;
  GFile *file = g_file_new_for_uri (uri);

  if (g_hash_table_lookup (priv->file_monitors, uri) == NULL)
    {
      GFileMonitor *monitor = g_file_monitor_file (file, G_FILE_MONITOR_NONE,
                                                   NULL, NULL);
      g_signal_connect (monitor, "changed",
                        G_CALLBACK (file_changed_cb), cache);
      g_hash_table_insert (priv->file_monitors, g_strdup (uri), monitor);
    }
  g_object_unref (file);
}

typedef struct {
  gchar *path;
  gint   grid_width, grid_height;
  ClutterActor *actor;
} AsyncImageData;

static void
on_data_destroy (gpointer data)
{
  AsyncImageData *d = (AsyncImageData *)data;
  g_free (d->path);
  g_object_unref (d->actor);
  g_free (d);
}

static void
on_sliced_image_loaded (GObject *source_object,
                        GAsyncResult *res,
                        gpointer user_data)
{
  AsyncImageData *data = (AsyncImageData *)user_data;
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  GList *list;

  if (g_simple_async_result_propagate_error (simple, NULL))
    return;

  for (list = g_simple_async_result_get_op_res_gpointer (simple); list; list = g_list_next (list))
    {
      ClutterActor *actor = load_from_pixbuf (GDK_PIXBUF (list->data));
      clutter_actor_hide (actor);
      clutter_actor_add_child (data->actor, actor);
    }
}

static void
free_glist_unref_gobjects (gpointer p)
{
  GList *list = p;
  GList *iter;

  for (iter = list; iter; iter = iter->next)
    g_object_unref (iter->data);
  g_list_free (list);
}

static void
load_sliced_image (GSimpleAsyncResult *result,
                   GObject *object,
                   GCancellable *cancellable)
{
  AsyncImageData *data;
  GList *res = NULL;
  GdkPixbuf *pix;
  gint width, height, y, x;

  g_assert (!cancellable);

  data = g_object_get_data (G_OBJECT (result), "load_sliced_image");
  g_assert (data);

  if (!(pix = gdk_pixbuf_new_from_file (data->path, NULL)))
    return;

  width = gdk_pixbuf_get_width (pix);
  height = gdk_pixbuf_get_height (pix);
  for (y = 0; y < height; y += data->grid_height)
    {
      for (x = 0; x < width; x += data->grid_width)
        {
          GdkPixbuf *pixbuf = gdk_pixbuf_new_subpixbuf (pix, x, y, data->grid_width, data->grid_height);
          g_assert (pixbuf != NULL);
          res = g_list_append (res, pixbuf);
        }
    }
  /* We don't need the original pixbuf anymore, though the subpixbufs
     will hold a reference. */
  g_object_unref (pix);
  g_simple_async_result_set_op_res_gpointer (result, res, free_glist_unref_gobjects);
}

/**
 * st_texture_cache_load_sliced_image:
 * @cache: A #StTextureCache
 * @path: Path to a filename
 * @grid_width: Width in pixels
 * @grid_height: Height in pixels
 *
 * This function reads a single image file which contains multiple images internally.
 * The image file will be divided using @grid_width and @grid_height;
 * note that the dimensions of the image loaded from @path 
 * should be a multiple of the specified grid dimensions.
 *
 * Returns: (transfer none): A new #ClutterActor
 */
ClutterActor *
st_texture_cache_load_sliced_image (StTextureCache    *cache,
                                    const gchar       *path,
                                    gint               grid_width,
                                    gint               grid_height)
{
  AsyncImageData *data;
  GSimpleAsyncResult *result;
  ClutterActor *actor = clutter_actor_new ();

  data = g_new0 (AsyncImageData, 1);
  data->grid_width = grid_width;
  data->grid_height = grid_height;
  data->path = g_strdup (path);
  data->actor = actor;
  g_object_ref (G_OBJECT (actor));

  result = g_simple_async_result_new (G_OBJECT (cache), on_sliced_image_loaded, data, st_texture_cache_load_sliced_image);

  g_object_set_data_full (G_OBJECT (result), "load_sliced_image", data, on_data_destroy);
  g_simple_async_result_run_in_thread (result, load_sliced_image, G_PRIORITY_DEFAULT, NULL);

  g_object_unref (result);

  return actor;
}

/**
 * st_texture_cache_load_uri_async:
 * @cache: The texture cache instance
 * @uri: uri of the image file from which to create a pixbuf
 * @available_width: available width for the image, can be -1 if not limited
 * @available_height: available height for the image, can be -1 if not limited
 *
 * Asynchronously load an image.   Initially, the returned texture will have a natural
 * size of zero.  At some later point, either the image will be loaded successfully
 * and at that point size will be negotiated, or upon an error, no image will be set.
 *
 * Return value: (transfer none): A new #ClutterActor with no image loaded initially.
 */
ClutterActor *
st_texture_cache_load_uri_async (StTextureCache *cache,
                                 const gchar    *uri,
                                 int             available_width,
                                 int             available_height)
{
  ClutterActor *texture;
  AsyncTextureLoadData *request;
  StTextureCachePolicy policy;
  gchar *key;

  key = g_strconcat (CACHE_PREFIX_URI, uri, NULL);

  policy = ST_TEXTURE_CACHE_POLICY_NONE; /* XXX */

  texture = (ClutterActor *) create_default_texture ();

  if (ensure_request (cache, key, policy, &request, texture))
    {
      /* If there's an outstanding request, we've just added ourselves to it */
      g_free (key);
    }
  else
    {
      /* Else, make a new request */

      request->cache = cache;
      /* Transfer ownership of key */
      request->key = key;
      request->uri = g_strdup (uri);
      request->policy = policy;
      request->width = available_width;
      request->height = available_height;

      load_texture_async (cache, request);
    }

  ensure_monitor_for_uri (cache, uri);

  return CLUTTER_ACTOR (texture);
}

static CoglHandle
st_texture_cache_load_uri_sync_to_cogl_texture (StTextureCache *cache,
                                                StTextureCachePolicy policy,
                                                const gchar    *uri,
                                                int             available_width,
                                                int             available_height,
                                                GError         **error)
{
  CoglHandle texdata;
  GdkPixbuf *pixbuf;
  char *key;

  key = g_strconcat (CACHE_PREFIX_URI, uri, NULL);

  texdata = g_hash_table_lookup (cache->priv->keyed_cache, key);

  if (texdata == NULL)
    {
      pixbuf = impl_load_pixbuf_file (uri, available_width, available_height, error);
      if (!pixbuf)
        goto out;

      texdata = pixbuf_to_cogl_handle (pixbuf, FALSE);
      g_object_unref (pixbuf);

      if (policy == ST_TEXTURE_CACHE_POLICY_FOREVER)
        {
          cogl_handle_ref (texdata);
          g_hash_table_insert (cache->priv->keyed_cache, g_strdup (key), texdata);
        }
    }
  else
    cogl_handle_ref (texdata);

  ensure_monitor_for_uri (cache, uri);

out:
  g_free (key);
  return texdata;
}

static cairo_surface_t *
st_texture_cache_load_uri_sync_to_cairo_surface (StTextureCache        *cache,
                                                 StTextureCachePolicy   policy,
                                                 const gchar           *uri,
                                                 int                    available_width,
                                                 int                    available_height,
                                                 GError               **error)
{
  cairo_surface_t *surface;
  GdkPixbuf *pixbuf;
  char *key;

  key = g_strconcat (CACHE_PREFIX_URI_FOR_CAIRO, uri, NULL);

  surface = g_hash_table_lookup (cache->priv->keyed_cache, key);

  if (surface == NULL)
    {
      pixbuf = impl_load_pixbuf_file (uri, available_width, available_height, error);
      if (!pixbuf)
        goto out;

      surface = pixbuf_to_cairo_surface (pixbuf);
      g_object_unref (pixbuf);

      if (policy == ST_TEXTURE_CACHE_POLICY_FOREVER)
        {
          cairo_surface_reference (surface);
          g_hash_table_insert (cache->priv->keyed_cache, g_strdup (key), surface);
        }
    }
  else
    cairo_surface_reference (surface);

  ensure_monitor_for_uri (cache, uri);

out:
  g_free (key);
  return surface;
}

/**
 * st_texture_cache_load_file_to_cogl_texture:
 * @cache: A #StTextureCache
 * @file_path: Path to a file in supported image format
 *
 * This function synchronously loads the given file path
 * into a COGL texture.  On error, a warning is emitted
 * and %COGL_INVALID_HANDLE is returned.
 *
 * Returns: (transfer full): a new #CoglHandle
 */
CoglHandle
st_texture_cache_load_file_to_cogl_texture (StTextureCache *cache,
                                            const gchar    *file_path)
{
  CoglHandle texture;
  GFile *file;
  char *uri;
  GError *error = NULL;

  file = g_file_new_for_path (file_path);
  uri = g_file_get_uri (file);

  texture = st_texture_cache_load_uri_sync_to_cogl_texture (cache, ST_TEXTURE_CACHE_POLICY_FOREVER,
                                                            uri, -1, -1, &error);
  g_object_unref (file);
  g_free (uri);

  if (texture == NULL)
    {
      g_warning ("Failed to load %s: %s", file_path, error->message);
      g_clear_error (&error);
      return COGL_INVALID_HANDLE;
    }
  return texture;
}

/**
 * st_texture_cache_load_file_to_cairo_surface:
 * @cache: A #StTextureCache
 * @file_path: Path to a file in supported image format
 *
 * This function synchronously loads the given file path
 * into a cairo surface.  On error, a warning is emitted
 * and %NULL is returned.
 *
 * Returns: (transfer full): a new #cairo_surface_t
 */
cairo_surface_t *
st_texture_cache_load_file_to_cairo_surface (StTextureCache *cache,
                                             const gchar    *file_path)
{
  cairo_surface_t *surface;
  GFile *file;
  char *uri;
  GError *error = NULL;

  file = g_file_new_for_path (file_path);
  uri = g_file_get_uri (file);

  surface = st_texture_cache_load_uri_sync_to_cairo_surface (cache, ST_TEXTURE_CACHE_POLICY_FOREVER,
                                                             uri, -1, -1, &error);
  g_object_unref (file);
  g_free (uri);

  if (surface == NULL)
    {
      g_warning ("Failed to load %s: %s", file_path, error->message);
      g_clear_error (&error);
      return NULL;
    }
  return surface;
}

/**
 * st_texture_cache_load_from_raw:
 * @cache: a #StTextureCache
 * @data: (array length=len): raw pixel data
 * @len: the length of @data
 * @has_alpha: whether @data includes an alpha channel
 * @width: width in pixels of @data
 * @height: width in pixels of @data
 * @rowstride: rowstride of @data
 * @size: size of icon to return
 *
 * Creates (or retrieves from cache) an icon based on raw pixel data.
 *
 * Return value: (transfer none): a new #ClutterActor displaying a
 * pixbuf created from @data and the other parameters.
 **/
ClutterActor *
st_texture_cache_load_from_raw (StTextureCache    *cache,
                                const guchar      *data,
                                gsize              len,
                                gboolean           has_alpha,
                                int                width,
                                int                height,
                                int                rowstride,
                                int                size,
                                GError           **error)
{
  ClutterTexture *texture;
  CoglHandle texdata;
  char *key;
  char *checksum;

  texture = create_default_texture ();
  clutter_actor_set_size (CLUTTER_ACTOR (texture), size, size);

  /* In theory, two images of with different width and height could have the same
   * pixel data and thus hash the same. (Say, a 16x16 and a 8x32 blank image.)
   * We ignore this for now. If anybody hits this problem they should use
   * GChecksum directly to compute a checksum including the width and height.
   */
  checksum = g_compute_checksum_for_data (G_CHECKSUM_SHA1, data, len);
  key = g_strdup_printf (CACHE_PREFIX_RAW_CHECKSUM "checksum=%s", checksum);
  g_free (checksum);

  texdata = g_hash_table_lookup (cache->priv->keyed_cache, key);
  if (texdata == NULL)
    {
      texdata = data_to_cogl_handle (data, has_alpha, width, height, rowstride, TRUE);
      g_hash_table_insert (cache->priv->keyed_cache, g_strdup (key), texdata);
    }

  g_free (key);

  set_texture_cogl_texture (texture, texdata);
  return CLUTTER_ACTOR (texture);
}

static StTextureCache *instance = NULL;

/**
 * st_texture_cache_get_default:
 *
 * Return value: (transfer none): The global texture cache
 */
StTextureCache*
st_texture_cache_get_default (void)
{
  if (instance == NULL)
    instance = g_object_new (ST_TYPE_TEXTURE_CACHE, NULL);
  return instance;
}
