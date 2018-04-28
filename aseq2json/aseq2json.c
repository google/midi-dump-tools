/*
 * Copyright 2014 Google Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <alloca.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include <asoundlib.h>
#include <gio/gunixoutputstream.h>
#include <glib.h>
#include <json-glib/json-glib.h>

static JsonNode* json_string_printf(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  gchar* tmp = g_strdup_vprintf(fmt, args);
  va_end(args);

  JsonNode* str = json_node_new(JSON_NODE_VALUE);
  json_node_set_string(str, tmp);
  g_free(tmp);
  return str;
}

static void consume_json_object(JsonGenerator* generator,
				JsonNode* json_node,
				JsonObject* obj,
				GOutputStream* out) {
  json_node_take_object(json_node, obj);
  if (json_object_get_size(obj)) {
    json_generator_set_root(generator, json_node);
    json_generator_to_stream(generator, out, NULL, NULL);
    g_output_stream_write(out, "\n", 1, NULL, NULL);
  }
}

static void add_json_event_1(JsonObject* obj,
			     const struct timespec* monotonic,
			     const struct timespec* realtime,
			     const char* event_type,
			     int src_client,
			     int src_port) {
  struct tm real_tm;
  gmtime_r(&realtime->tv_sec, &real_tm);

  json_object_set_member(obj, "time",
			 json_string_printf("%04d-%02d-%02dT%02d:"
					    "%02d:%02d.%09ldZ",
					    real_tm.tm_year + 1900,
					    real_tm.tm_mon + 1,
					    real_tm.tm_mday,
					    real_tm.tm_hour, real_tm.tm_min,
					    real_tm.tm_sec,
					    realtime->tv_nsec));
  json_object_set_double_member(obj, "timestamp",
				(double) monotonic->tv_sec +
				(double) monotonic->tv_nsec / 1000000000.0);
  json_object_set_string_member(obj, "eventType", event_type);
  json_object_set_member(obj, "source", json_string_printf("%d:%d",
							   src_client,
							   src_port));
}

static void subscribe(snd_seq_t* seq, int client, int port,
		      int dest_client, int dest_port,
		      const char* client_name,
		      const char* port_name,
		      JsonGenerator* generator,
		      JsonNode* json_node,
		      const struct timespec* monotonic,
		      const struct timespec* realtime,
		      GOutputStream* out,
		      GHashTable* active_clients) {
  snd_seq_addr_t sender, dest;
  snd_seq_port_subscribe_t* subs;
  sender.client = client;
  sender.port = port;
  dest.client = dest_client;
  dest.port = dest_port;
  snd_seq_port_subscribe_alloca(&subs);
  snd_seq_port_subscribe_set_sender(subs, &sender);
  snd_seq_port_subscribe_set_dest(subs, &dest);
  int err = snd_seq_subscribe_port(seq, subs);
  // TODO(agoode): Detect and recover from temporary error here.
  if (err < 0)
    return;

  // Don't record or announce the announce port.
  if ((client == SND_SEQ_CLIENT_SYSTEM) &&
      (port == SND_SEQ_PORT_SYSTEM_ANNOUNCE))
    return;

  // Mark active.
  gpointer client_key = GINT_TO_POINTER(client);
  gpointer port_key = GINT_TO_POINTER(port);
  GHashTable* active_ports;
  if (g_hash_table_contains(active_clients, client_key)) {
    active_ports = g_hash_table_lookup(active_clients, client_key);
  } else {
    active_ports = g_hash_table_new(NULL, NULL);
    g_hash_table_insert(active_clients, client_key, active_ports);
  }
  // Check for dup. This can happen from hotplugging, when we get the
  // subscription after initial scan.
  if (g_hash_table_contains(active_ports, port_key))
    return;
  // Insert the port.
  g_hash_table_insert(active_ports, port_key, NULL);

  // Announce the new port.
  JsonObject* obj = json_object_new();
  add_json_event_1(obj, monotonic, realtime, "source-start", client, port);
  json_object_set_string_member(obj, "clientName", client_name);
  json_object_set_string_member(obj, "name", port_name);
  consume_json_object(generator, json_node, obj, out);
}

static void deactivate_port(int client, int port,
			    JsonGenerator* generator,
			    JsonNode* json_node,
			    const struct timespec* monotonic,
			    const struct timespec* realtime,
			    GOutputStream* out,
			    GHashTable* active_ports) {
  gpointer port_key = GINT_TO_POINTER(port);
  if (g_hash_table_remove(active_ports, port_key)) {
    JsonObject* obj = json_object_new();
    add_json_event_1(obj, monotonic, realtime, "source-exit", client, port);
    consume_json_object(generator, json_node, obj, out);
  }
}

static void deactivate_client(int client,
			      JsonGenerator* generator,
			      JsonNode* json_node,
			      const struct timespec* monotonic,
			      const struct timespec* realtime,
			      GOutputStream* out,
			      GHashTable* active_clients) {
  gpointer client_key = GINT_TO_POINTER(client);
  GHashTable* active_ports = g_hash_table_lookup(active_clients,
						 client_key);
  if (!active_ports)
    return;

  GList* keys = g_hash_table_get_keys(active_ports);
  while (keys) {
    deactivate_port(client, GPOINTER_TO_INT(keys->data),
		    generator, json_node,
		    monotonic, realtime, out, active_ports);
    keys = g_list_delete_link(keys, keys);
  }
  g_hash_table_remove(active_clients, client_key);
}

static void add_channel_note_velocity(JsonObject* obj,
				      const snd_seq_ev_note_t* note) {
  json_object_set_int_member(obj, "channel", note->channel);
  json_object_set_int_member(obj, "note", note->note);
  json_object_set_int_member(obj, "velocity", note->velocity);
}

static void add_control_1(JsonObject* obj,
			  const snd_seq_ev_ctrl_t* control) {
  json_object_set_int_member(obj, "channel", control->channel);
  json_object_set_int_member(obj, "value", control->value);
}

static void add_control_2(JsonObject* obj,
			  const snd_seq_ev_ctrl_t* control) {
  add_control_1(obj, control);
  json_object_set_int_member(obj, "param", control->param);
}

static void add_raw(JsonObject* obj,
		    const snd_seq_ev_ext_t* ext) {
  JsonArray* array = json_array_sized_new(ext->len);
  uint8_t* data = ext->ptr;
  for (unsigned int i = 0; i < ext->len; i++)
    json_array_add_int_element(array, data[i]);
  json_object_set_array_member(obj, "raw", array);
}

static void add_json_event(JsonObject* obj,
			   const struct timespec* m,
			   const struct timespec* r,
			   const snd_seq_event_t* ev) {
  switch (ev->type) {
  case SND_SEQ_EVENT_NOTEOFF:
    add_json_event_1(obj, m, r, "note-off",
		     ev->source.client, ev->source.port);
    add_channel_note_velocity(obj, &ev->data.note);
    break;

  case SND_SEQ_EVENT_NOTEON:
    add_json_event_1(obj, m, r, "note-on",
		     ev->source.client, ev->source.port);
    add_channel_note_velocity(obj, &ev->data.note);
    break;

  case SND_SEQ_EVENT_KEYPRESS:
    add_json_event_1(obj, m, r, "polyphonic-key-pressure",
		     ev->source.client, ev->source.port);
    add_channel_note_velocity(obj, &ev->data.note);
    break;

  case SND_SEQ_EVENT_CONTROLLER:
    add_json_event_1(obj, m, r, "control-change",
		     ev->source.client, ev->source.port);
    add_control_2(obj, &ev->data.control);
    break;

  case SND_SEQ_EVENT_PGMCHANGE:
    add_json_event_1(obj, m, r, "program-change",
		     ev->source.client, ev->source.port);
    add_control_1(obj, &ev->data.control);
    break;

  case SND_SEQ_EVENT_CHANPRESS:
    add_json_event_1(obj, m, r, "channel-pressure",
		     ev->source.client, ev->source.port);
    add_control_1(obj, &ev->data.control);
    break;

  case SND_SEQ_EVENT_PITCHBEND:
    add_json_event_1(obj, m, r, "pitch-bend-change",
		     ev->source.client, ev->source.port);
    add_control_1(obj, &ev->data.control);
    break;

  case SND_SEQ_EVENT_CONTROL14:
    add_json_event_1(obj, m, r, "control-change-14",
		     ev->source.client, ev->source.port);
    add_control_2(obj, &ev->data.control);
    break;

  case SND_SEQ_EVENT_NONREGPARAM:
    add_json_event_1(obj, m, r, "non-registered-parameter-number",
		     ev->source.client, ev->source.port);
    add_control_2(obj, &ev->data.control);
    break;

  case SND_SEQ_EVENT_REGPARAM:
    add_json_event_1(obj, m, r, "registered-parameter-number",
		     ev->source.client, ev->source.port);
    add_control_2(obj, &ev->data.control);
    break;

  case SND_SEQ_EVENT_SONGPOS:
    add_json_event_1(obj, m, r, "song-position-pointer",
		     ev->source.client, ev->source.port);
    add_control_1(obj, &ev->data.control);
    break;

  case SND_SEQ_EVENT_SONGSEL:
    add_json_event_1(obj, m, r, "song-select",
		     ev->source.client, ev->source.port);
    add_control_1(obj, &ev->data.control);
    break;

  case SND_SEQ_EVENT_QFRAME:
    add_json_event_1(obj, m, r, "midi-time-code-quarter-frame",
		     ev->source.client, ev->source.port);
    add_control_1(obj, &ev->data.control);
    break;

  case SND_SEQ_EVENT_START:
    add_json_event_1(obj, m, r, "start",
		     ev->source.client, ev->source.port);
    break;

  case SND_SEQ_EVENT_CONTINUE:
    add_json_event_1(obj, m, r, "continue",
		     ev->source.client, ev->source.port);
    break;

  case SND_SEQ_EVENT_STOP:
    add_json_event_1(obj, m, r, "stop",
		     ev->source.client, ev->source.port);
    break;

  case SND_SEQ_EVENT_CLOCK:
    add_json_event_1(obj, m, r, "timing-clock",
		     ev->source.client, ev->source.port);
    break;

  case SND_SEQ_EVENT_TUNE_REQUEST:
    add_json_event_1(obj, m, r, "tune-request",
		     ev->source.client, ev->source.port);
    break;

  case SND_SEQ_EVENT_RESET:
    add_json_event_1(obj, m, r, "reset",
		     ev->source.client, ev->source.port);
    break;

  case SND_SEQ_EVENT_SENSING:
    add_json_event_1(obj, m, r, "active-sensing",
		     ev->source.client, ev->source.port);
    break;

  case SND_SEQ_EVENT_SYSEX:
    add_json_event_1(obj, m, r, "system-exclusive",
		     ev->source.client, ev->source.port);
    add_raw(obj, &ev->data.ext);
    break;
  }
}

static void enumerate_ports(snd_seq_t* seq, int client_id, int dest_port,
			    JsonGenerator* generator, JsonNode* json_node,
			    GOutputStream*out, GHashTable* active_clients) {
  snd_seq_client_info_t* client_info;
  snd_seq_port_info_t* port_info;
  snd_seq_client_info_alloca(&client_info);
  snd_seq_port_info_alloca(&port_info);

  struct timespec monotonic;
  struct timespec realtime;
  clock_gettime(CLOCK_MONOTONIC, &monotonic);
  clock_gettime(CLOCK_REALTIME, &realtime);

  // Enumerate all clients.
  snd_seq_client_info_set_client(client_info, -1);
  while (snd_seq_query_next_client(seq, client_info) == 0) {
    int client = snd_seq_client_info_get_client(client_info);
    // Skip if our own client.
    if (client_id == client)
      continue;

    // Enumerate all ports.
    snd_seq_port_info_set_client(port_info, client);
    snd_seq_port_info_set_port(port_info, -1);
    const char* client_name = snd_seq_client_info_get_name(client_info);
    while (snd_seq_query_next_port(seq, port_info) == 0) {
      int port = snd_seq_port_info_get_port(port_info);
      unsigned int capability = snd_seq_port_info_get_capability(port_info);
      unsigned int type = snd_seq_port_info_get_type(port_info);
      const char* port_name = snd_seq_port_info_get_name(port_info);

      if ((capability & SND_SEQ_PORT_CAP_READ) &&
	  (capability & SND_SEQ_PORT_CAP_SUBS_READ) &&
	  (type & SND_SEQ_PORT_TYPE_MIDI_GENERIC))
	subscribe(seq, client, port, client_id, dest_port,
		  client_name, port_name, generator, json_node,
		  &monotonic, &realtime, out,
		  active_clients);
    }
  }
}

typedef struct {
  struct timespec m;
  struct timespec r;
  snd_seq_event_t ev;
} queue_entry;

typedef struct {
  GAsyncQueue* q;
  int client_id;
  int dest_port;
  snd_seq_t* seq;
  JsonGenerator* generator;
  JsonNode* json_node;
  GOutputStream* out;
  GHashTable* active_clients;
} state;

static gpointer event_loop(gpointer data) {
  state* s = data;

  snd_seq_client_info_t* client_info;
  snd_seq_port_info_t* port_info;
  snd_seq_client_info_alloca(&client_info);
  snd_seq_port_info_alloca(&port_info);

  while (true) {
    queue_entry* e = g_async_queue_pop(s->q);
    JsonObject* obj;

    if ((e->ev.source.client == SND_SEQ_CLIENT_SYSTEM) &&
	(e->ev.source.port == SND_SEQ_PORT_SYSTEM_ANNOUNCE)) {
      int client;
      int port;
      switch (e->ev.type) {
      case SND_SEQ_EVENT_PORT_START:
	// Subscribe to the new port, if applicable.
	client = e->ev.data.addr.client;
	port = e->ev.data.addr.port;
	if (client == s->client_id)
	  break;
	int err;
	err = snd_seq_get_any_client_info(s->seq, client, client_info);
	if (err != 0)
	  break;
	err = snd_seq_get_any_port_info(s->seq, client, port, port_info);
	if (err != 0)
	  break;

	int capability = snd_seq_port_info_get_capability(port_info);
	int type = snd_seq_port_info_get_type(port_info);
	if ((capability & SND_SEQ_PORT_CAP_READ) &&
	    (capability & SND_SEQ_PORT_CAP_SUBS_READ) &&
	    (type & SND_SEQ_PORT_TYPE_MIDI_GENERIC)) {
	  subscribe(s->seq, client, port, s->client_id, s->dest_port,
		    snd_seq_client_info_get_name(client_info),
		    snd_seq_port_info_get_name(port_info),
		    s->generator, s->json_node, &e->m, &e->r, s->out,
		    s->active_clients);
	}
	break;

      case SND_SEQ_EVENT_PORT_EXIT:
	client = e->ev.data.addr.client;
	if (g_hash_table_contains(s->active_clients, GINT_TO_POINTER(client)))
	  deactivate_port(client, e->ev.data.addr.port,
			  s->generator, s->json_node, &e->m, &e->r, s->out,
			  g_hash_table_lookup(s->active_clients,
					      GINT_TO_POINTER(client)));
	break;

      case SND_SEQ_EVENT_CLIENT_EXIT:
	deactivate_client(e->ev.data.addr.client, s->generator, s->json_node,
			  &e->m, &e->r, s->out, s->active_clients);
	break;
      }
    } else {
      obj = json_object_new();
      add_json_event(obj, &e->m, &e->r, &e->ev);
      consume_json_object(s->generator, s->json_node, obj, s->out);
    }
    g_slice_free(queue_entry, e);
  }
  return NULL;
}

int main(void) {
  state s;
  s.q = g_async_queue_new();
  s.active_clients =
    g_hash_table_new_full(NULL, NULL, NULL,
			  (GDestroyNotify) g_hash_table_destroy);

  int err;

  // Connect client.
  err = snd_seq_open(&s.seq, "default", SND_SEQ_OPEN_INPUT, 0);
  if (err)
    return EXIT_FAILURE;
  err = snd_seq_set_client_name(s.seq, "aseq2json");
  if (err)
    return EXIT_FAILURE;
  s.client_id = snd_seq_client_id(s.seq);

  // Set up port.
  s.dest_port = snd_seq_create_simple_port(s.seq, "universal listener",
					   SND_SEQ_PORT_CAP_WRITE |
					   SND_SEQ_PORT_CAP_NO_EXPORT,
					   SND_SEQ_PORT_TYPE_MIDI_GENERIC |
					   SND_SEQ_PORT_TYPE_APPLICATION);
  if (s.dest_port < 0)
    return EXIT_FAILURE;

  // Init output.
  s.generator = json_generator_new();
  //  json_generator_set_pretty(s.generator, true);
  s.json_node = json_node_new(JSON_NODE_OBJECT);
  s.out = g_unix_output_stream_new(STDOUT_FILENO, false);

  // Listen on announce port.
  subscribe(s.seq, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE,
	    s.client_id, s.dest_port,
	    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	    s.active_clients);

  enumerate_ports(s.seq, s.client_id, s.dest_port, s.generator, s.json_node,
		  s.out, s.active_clients);

  // Start event processor.
  g_thread_new(NULL, event_loop, &s);

  // Listen for all events.
  snd_seq_client_info_t* client_info;
  snd_seq_port_info_t* port_info;
  snd_seq_client_info_alloca(&client_info);
  snd_seq_port_info_alloca(&port_info);
  while (true) {
    snd_seq_event_t* ev;
    int i = snd_seq_event_input(s.seq, &ev);
    if (i == -ENOSPC) {
      g_warning("snd_seq_event_input: lost event");
      continue;
    } else if (i < 0) {
      // TODO(agoode): reinitialize?
      g_error("failure returned from snd_seq_event_input: %s", snd_strerror(i));
    }
    queue_entry* entry = g_slice_new(queue_entry);
    entry->ev = *ev;

    // Get times.
    clock_gettime(CLOCK_MONOTONIC, &entry->m);
    clock_gettime(CLOCK_REALTIME, &entry->r);

    // Enquque this.
    g_async_queue_push(s.q, entry);
  }
  return EXIT_SUCCESS;
}
