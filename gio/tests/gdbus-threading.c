/* GLib testing framework examples and tests
 *
 * Copyright (C) 2008-2010 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <gio/gio.h>
#include <unistd.h>
#include <string.h>

#include "gdbusprivate.h"
#include "gdbus-tests.h"

/* all tests rely on a global connection */
static GDBusConnection *c = NULL;

typedef struct
{
  GMainContext *context;
  gboolean timed_out;
} TimeoutData;

static gboolean
timeout_cb (gpointer user_data)
{
  TimeoutData *data = user_data;

  data->timed_out = TRUE;
  g_main_context_wakeup (data->context);

  return G_SOURCE_REMOVE;
}

static gboolean
wakeup_cb (gpointer user_data)
{
  /* nothing to do here */
  return G_SOURCE_CONTINUE;
}

/* Check that the given @connection has only one ref, waiting to let any pending
 * unrefs complete first. This is typically used on the shared connection, to
 * ensure it’s in a correct state before beginning the next test. */
static void
(assert_connection_has_one_ref) (GDBusConnection *connection,
                                 GMainContext    *context,
                                 const gchar     *calling_function)
{
  GSource *timeout_source = NULL;
  GSource *wakeup_source = NULL;
  TimeoutData data = { context, FALSE };

  if (g_atomic_int_get (&G_OBJECT (connection)->ref_count) == 1)
    return;

  /* Use two timeout sources: @timeout_source to set a deadline after which the
   * test will fail if the @connection doesn’t have the right number of refs;
   * and @wakeup_source to periodically wake the @context up to allow the
   * termination condition to be checked. This allows the termination condition
   * to be fulfilled by something which doesn’t wake @context up, such as an
   * unref happening in the GDBus worker thread. */
  timeout_source = g_timeout_source_new_seconds (3);
  g_source_set_callback (timeout_source, timeout_cb, &data, NULL);
  g_source_attach (timeout_source, context);

  wakeup_source = g_timeout_source_new (50 /* ms */);
  g_source_set_callback (wakeup_source, wakeup_cb, NULL, NULL);
  g_source_attach (wakeup_source, context);

  while (g_atomic_int_get (&G_OBJECT (connection)->ref_count) != 1 && !data.timed_out)
    {
      g_debug ("refcount of %p is not right (%u rather than 1) in %s(), sleeping",
               connection, g_atomic_int_get (&G_OBJECT (connection)->ref_count), calling_function);
      g_main_context_iteration (NULL, TRUE);
    }

  g_source_destroy (wakeup_source);
  g_source_unref (wakeup_source);

  g_source_destroy (timeout_source);
  g_source_unref (timeout_source);

  if (g_atomic_int_get (&G_OBJECT (connection)->ref_count) != 1)
    g_error ("connection %p had too many refs (%u rather than 1) in %s()",
             connection, g_atomic_int_get (&G_OBJECT (connection)->ref_count), calling_function);
}

/* Macro wrapper to add in the calling function name */
#define assert_connection_has_one_ref(connection, context) \
  (assert_connection_has_one_ref) (connection, context, G_STRFUNC)

/* ---------------------------------------------------------------------------------------------------- */
/* Ensure that signal and method replies are delivered in the right thread */
/* ---------------------------------------------------------------------------------------------------- */

typedef struct {
  GThread *thread;
  GMainContext *context;
  guint signal_count;
  gboolean unsubscribe_complete;
  GAsyncResult *async_result;
} DeliveryData;

static void
async_result_cb (GDBusConnection *connection,
                 GAsyncResult    *res,
                 gpointer         user_data)
{
  DeliveryData *data = user_data;

  data->async_result = g_object_ref (res);

  g_assert_true (g_thread_self () == data->thread);

  g_main_context_wakeup (data->context);
}

static void
signal_handler (GDBusConnection *connection,
                const gchar      *sender_name,
                const gchar      *object_path,
                const gchar      *interface_name,
                const gchar      *signal_name,
                GVariant         *parameters,
                gpointer         user_data)
{
  DeliveryData *data = user_data;

  g_assert_true (g_thread_self () == data->thread);

  data->signal_count++;

  g_main_context_wakeup (data->context);
}

static void
signal_data_free_cb (gpointer user_data)
{
  DeliveryData *data = user_data;

  g_assert_true (g_thread_self () == data->thread);

  data->unsubscribe_complete = TRUE;

  g_main_context_wakeup (data->context);
}

static gpointer
test_delivery_in_thread_func (gpointer _data)
{
  GMainContext *thread_context;
  DeliveryData data;
  GCancellable *ca;
  guint subscription_id;
  GError *error = NULL;
  GVariant *result_variant = NULL;

  thread_context = g_main_context_new ();
  g_main_context_push_thread_default (thread_context);

  data.thread = g_thread_self ();
  data.context = thread_context;
  data.signal_count = 0;
  data.unsubscribe_complete = FALSE;
  data.async_result = NULL;

  /* ---------------------------------------------------------------------------------------------------- */

  /*
   * Check that we get a reply to the GetId() method call.
   */
  g_dbus_connection_call (c,
                          DBUS_SERVICE_DBUS,
                          DBUS_PATH_DBUS,
                          DBUS_INTERFACE_DBUS,
                          "GetId",                 /* method name */
                          NULL, NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          NULL,
                          (GAsyncReadyCallback) async_result_cb,
                          &data);
  while (data.async_result == NULL)
    g_main_context_iteration (thread_context, TRUE);

  result_variant = g_dbus_connection_call_finish (c, data.async_result, &error);
  g_assert_no_error (error);
  g_assert_nonnull (result_variant);
  g_clear_pointer (&result_variant, g_variant_unref);
  g_clear_object (&data.async_result);

  /*
   * Check that we never actually send a message if the GCancellable
   * is already cancelled - i.e.  we should get G_IO_ERROR_CANCELLED
   * when the actual connection is not up.
   */
  ca = g_cancellable_new ();
  g_cancellable_cancel (ca);
  g_dbus_connection_call (c,
                          DBUS_SERVICE_DBUS,
                          DBUS_PATH_DBUS,
                          DBUS_INTERFACE_DBUS,
                          "GetId",                 /* method name */
                          NULL, NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          ca,
                          (GAsyncReadyCallback) async_result_cb,
                          &data);
  while (data.async_result == NULL)
    g_main_context_iteration (thread_context, TRUE);

  result_variant = g_dbus_connection_call_finish (c, data.async_result, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_false (g_dbus_error_is_remote_error (error));
  g_clear_error (&error);
  g_assert_null (result_variant);
  g_clear_object (&data.async_result);

  g_object_unref (ca);

  /*
   * Check that cancellation works when the message is already in flight.
   */
  ca = g_cancellable_new ();
  g_dbus_connection_call (c,
                          DBUS_SERVICE_DBUS,
                          DBUS_PATH_DBUS,
                          DBUS_INTERFACE_DBUS,
                          "GetId",                 /* method name */
                          NULL, NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          ca,
                          (GAsyncReadyCallback) async_result_cb,
                          &data);
  g_cancellable_cancel (ca);

  while (data.async_result == NULL)
    g_main_context_iteration (thread_context, TRUE);

  result_variant = g_dbus_connection_call_finish (c, data.async_result, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_false (g_dbus_error_is_remote_error (error));
  g_clear_error (&error);
  g_assert_null (result_variant);
  g_clear_object (&data.async_result);

  g_object_unref (ca);

  /*
   * Check that signals are delivered to the correct thread.
   *
   * First we subscribe to the signal, then we call EmitSignal(). This should
   * cause a TestSignal emission from the testserver.
   */
  subscription_id = g_dbus_connection_signal_subscribe (c,
                                                        "com.example.TestService", /* sender */
                                                        "com.example.Frob",        /* interface */
                                                        "TestSignal",              /* member */
                                                        "/com/example/TestObject", /* path */
                                                        NULL,
                                                        G_DBUS_SIGNAL_FLAGS_NONE,
                                                        signal_handler,
                                                        &data,
                                                        signal_data_free_cb);
  g_assert_cmpuint (subscription_id, !=, 0);
  g_assert_cmpuint (data.signal_count, ==, 0);

  g_dbus_connection_call (c,
                          "com.example.TestService", /* bus_name */
                          "/com/example/TestObject", /* object path */
                          "com.example.Frob",        /* interface name */
                          "EmitSignal",              /* method name */
                          g_variant_new_parsed ("('hello', @o '/com/example/TestObject')"),
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          NULL,
                          (GAsyncReadyCallback) async_result_cb,
                          &data);
  while (data.async_result == NULL || data.signal_count < 1)
    g_main_context_iteration (thread_context, TRUE);

  result_variant = g_dbus_connection_call_finish (c, data.async_result, &error);
  g_assert_no_error (error);
  g_assert_nonnull (result_variant);
  g_clear_pointer (&result_variant, g_variant_unref);
  g_clear_object (&data.async_result);

  g_assert_cmpuint (data.signal_count, ==, 1);

  g_dbus_connection_signal_unsubscribe (c, subscription_id);
  subscription_id = 0;

  while (!data.unsubscribe_complete)
    g_main_context_iteration (thread_context, TRUE);
  g_assert_true (data.unsubscribe_complete);

  /* ---------------------------------------------------------------------------------------------------- */

  g_main_context_pop_thread_default (thread_context);
  g_main_context_unref (thread_context);

  return NULL;
}

static void
test_delivery_in_thread (void)
{
  GThread *thread;

  thread = g_thread_new ("deliver",
                         test_delivery_in_thread_func,
                         NULL);

  g_thread_join (thread);

  assert_connection_has_one_ref (c, NULL);
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct {
  GDBusProxy *proxy;
  gint msec;
  guint num;
  gboolean async;

  GMainLoop *thread_loop;
  GThread *thread;
} SyncThreadData;

static void
sleep_cb (GDBusProxy   *proxy,
          GAsyncResult *res,
          gpointer      user_data)
{
  SyncThreadData *data = user_data;
  GError *error;
  GVariant *result;

  error = NULL;
  result = g_dbus_proxy_call_finish (proxy,
                                     res,
                                     &error);
  g_assert_no_error (error);
  g_assert_nonnull (result);
  g_assert_cmpstr (g_variant_get_type_string (result), ==, "()");
  g_variant_unref (result);

  g_assert_true (data->thread == g_thread_self ());

  g_main_loop_quit (data->thread_loop);

  //g_debug ("async cb (%p)", g_thread_self ());
}

static gpointer
test_sleep_in_thread_func (gpointer _data)
{
  SyncThreadData *data = _data;
  GMainContext *thread_context;
  guint n;

  thread_context = g_main_context_new ();
  data->thread_loop = g_main_loop_new (thread_context, FALSE);
  g_main_context_push_thread_default (thread_context);

  data->thread = g_thread_self ();

  for (n = 0; n < data->num; n++)
    {
      if (data->async)
        {
          //g_debug ("invoking async (%p)", g_thread_self ());
          g_dbus_proxy_call (data->proxy,
                             "Sleep",
                             g_variant_new ("(i)", data->msec),
                             G_DBUS_CALL_FLAGS_NONE,
                             -1,
                             NULL,
                             (GAsyncReadyCallback) sleep_cb,
                             data);
          g_main_loop_run (data->thread_loop);
          if (g_test_verbose ())
            g_printerr ("A");
          //g_debug ("done invoking async (%p)", g_thread_self ());
        }
      else
        {
          GError *error;
          GVariant *result;

          error = NULL;
          //g_debug ("invoking sync (%p)", g_thread_self ());
          result = g_dbus_proxy_call_sync (data->proxy,
                                           "Sleep",
                                           g_variant_new ("(i)", data->msec),
                                           G_DBUS_CALL_FLAGS_NONE,
                                           -1,
                                           NULL,
                                           &error);
          if (g_test_verbose ())
            g_printerr ("S");
          //g_debug ("done invoking sync (%p)", g_thread_self ());
          g_assert_no_error (error);
          g_assert_nonnull (result);
          g_assert_cmpstr (g_variant_get_type_string (result), ==, "()");
          g_variant_unref (result);
        }
    }

  g_main_context_pop_thread_default (thread_context);
  g_main_loop_unref (data->thread_loop);
  g_main_context_unref (thread_context);

  return NULL;
}

static void
test_method_calls_on_proxy (GDBusProxy *proxy)
{
  guint n, divisor;

  /*
   * Check that multiple threads can do calls without interfering with
   * each other. We do this by creating three threads that call the
   * Sleep() method on the server (which handles it asynchronously, e.g.
   * it won't block other requests) with different sleep durations and
   * a number of times. We do this so each set of calls add up to 4000
   * milliseconds.
   *
   * The dbus test server that this code calls into uses glib timeouts
   * to do the sleeping which have only a granularity of 1ms.  It is
   * therefore possible to lose as much as 40ms; the test could finish
   * in slightly less than 4 seconds.
   *
   * We run this test twice - first with async calls in each thread, then
   * again with sync calls
   */

  if (g_test_thorough ())
    divisor = 1;
  else
    divisor = 10;

  for (n = 0; n < 2; n++)
    {
      gboolean do_async;
      GThread *thread1;
      GThread *thread2;
      GThread *thread3;
      SyncThreadData data1;
      SyncThreadData data2;
      SyncThreadData data3;
      gint64 start_time, end_time;
      guint elapsed_msec;

      do_async = (n == 0);

      start_time = g_get_real_time ();

      data1.proxy = proxy;
      data1.msec = 40;
      data1.num = 100 / divisor;
      data1.async = do_async;
      thread1 = g_thread_new ("sleep",
                              test_sleep_in_thread_func,
                              &data1);

      data2.proxy = proxy;
      data2.msec = 20;
      data2.num = 200 / divisor;
      data2.async = do_async;
      thread2 = g_thread_new ("sleep2",
                              test_sleep_in_thread_func,
                              &data2);

      data3.proxy = proxy;
      data3.msec = 100;
      data3.num = 40 / divisor;
      data3.async = do_async;
      thread3 = g_thread_new ("sleep3",
                              test_sleep_in_thread_func,
                              &data3);

      g_thread_join (thread1);
      g_thread_join (thread2);
      g_thread_join (thread3);

      end_time = g_get_real_time ();

      elapsed_msec = (end_time - start_time) / 1000;

      //g_debug ("Elapsed time for %s = %d msec", n == 0 ? "async" : "sync", elapsed_msec);

      /* elapsed_msec should be 4000 msec +/- change for overhead/inaccuracy */
      g_assert_cmpint (elapsed_msec, >=, 3950 / divisor);
      g_assert_cmpint (elapsed_msec,  <, 30000 / divisor);

      if (g_test_verbose ())
        g_printerr (" ");
    }
}

static void
test_method_calls_in_thread (void)
{
  GDBusProxy *proxy;
  GDBusConnection *connection;
  GError *error;

  error = NULL;
  connection = g_bus_get_sync (G_BUS_TYPE_SESSION,
                               NULL,
                               &error);
  g_assert_no_error (error);
  error = NULL;
  proxy = g_dbus_proxy_new_sync (connection,
                                 G_DBUS_PROXY_FLAGS_NONE,
                                 NULL,                      /* GDBusInterfaceInfo */
                                 "com.example.TestService", /* name */
                                 "/com/example/TestObject", /* object path */
                                 "com.example.Frob",        /* interface */
                                 NULL, /* GCancellable */
                                 &error);
  g_assert_no_error (error);

  test_method_calls_on_proxy (proxy);

  g_object_unref (proxy);
  g_object_unref (connection);

  if (g_test_verbose ())
    g_printerr ("\n");

  assert_connection_has_one_ref (c, NULL);
}

#define SLEEP_MIN_USEC 1
#define SLEEP_MAX_USEC 10

/* Can run in any thread */
static void
ensure_connection_works (GDBusConnection *conn)
{
  GVariant *v;
  GError *error = NULL;

  v = g_dbus_connection_call_sync (conn, DBUS_SERVICE_DBUS,
      DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetId", NULL, NULL, 0, -1,
      NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (v);
  g_assert_true (g_variant_is_of_type (v, G_VARIANT_TYPE ("(s)")));
  g_variant_unref (v);
}

/**
 * get_sync_in_thread:
 * @data: (type guint): delay in microseconds
 *
 * Sleep for a short time, then get a session bus connection and call
 * a method on it.
 *
 * Runs in a non-main thread.
 *
 * Returns: (transfer full): the connection
 */
static gpointer
get_sync_in_thread (gpointer data)
{
  guint delay = GPOINTER_TO_UINT (data);
  GError *error = NULL;
  GDBusConnection *conn;

  g_usleep (delay);

  conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error (error);

  ensure_connection_works (conn);

  return conn;
}

static void
test_threaded_singleton (void)
{
  guint i, n;
  guint unref_wins = 0;
  guint get_wins = 0;

  if (g_test_thorough ())
    n = 100000;
  else
    n = 1000;

  for (i = 0; i < n; i++)
    {
      GThread *thread;
      guint unref_delay, get_delay;
      GDBusConnection *new_conn;

      /* We want to be the last ref, so let it finish setting up */
      assert_connection_has_one_ref (c, NULL);

      if (g_test_verbose () && (i % (n/50)) == 0)
        g_printerr ("%u%%\n", ((i * 100) / n));

      /* Delay for a random time on each side of the race, to perturb the
       * timing. Ideally, we want each side to win half the races; these
       * timings are about right on smcv's laptop.
       */
      unref_delay = g_random_int_range (SLEEP_MIN_USEC, SLEEP_MAX_USEC);
      get_delay = g_random_int_range (SLEEP_MIN_USEC / 2, SLEEP_MAX_USEC / 2);

      /* One half of the race is to call g_bus_get_sync... */
      thread = g_thread_new ("get_sync_in_thread", get_sync_in_thread,
          GUINT_TO_POINTER (get_delay));

      /* ... and the other half is to unref the shared connection, which must
       * have exactly one ref at this point
       */
      g_usleep (unref_delay);
      g_object_unref (c);

      /* Wait for the thread to run; see what it got */
      new_conn = g_thread_join (thread);

      /* If the thread won the race, it will have kept the same connection,
       * and it'll have one ref
       */
      if (new_conn == c)
        {
          get_wins++;
        }
      else
        {
          unref_wins++;
          /* c is invalid now, but new_conn is suitable for the
           * next round
           */
          c = new_conn;
        }

      ensure_connection_works (c);
    }

  if (g_test_verbose ())
    g_printerr ("Unref won %u races; Get won %u races\n", unref_wins, get_wins);
}

/* ---------------------------------------------------------------------------------------------------- */

int
main (int   argc,
      char *argv[])
{
  GError *error;
  gint ret;
  gchar *path;

  g_test_init (&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

  if (g_getenv ("DEB_ALLOW_FLAKY_TESTS") == NULL)
    {
      g_print("1..0 # SKIP all gdbus-threading tests skipped because they are too unreliable (glib#1515)\n");
      return 0;
    }

  session_bus_up ();

  /* this is safe; testserver will exit once the bus goes away */
  path = g_test_build_filename (G_TEST_BUILT, "gdbus-testserver", NULL);
  g_assert_true (g_spawn_command_line_async (path, NULL));
  g_free (path);

  /* Create the connection in the main thread */
  error = NULL;
  c = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (c);

  ensure_gdbus_testserver_up (c, NULL);

  g_test_add_func ("/gdbus/delivery-in-thread", test_delivery_in_thread);
  g_test_add_func ("/gdbus/method-calls-in-thread", test_method_calls_in_thread);
  g_test_add_func ("/gdbus/threaded-singleton", test_threaded_singleton);

  ret = g_test_run();

  g_object_unref (c);

  /* tear down bus */
  session_bus_down ();

  return ret;
}
