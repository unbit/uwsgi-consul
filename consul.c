#include <uwsgi.h>
#include <curl/curl.h>

// give the plugin access to the global uwsgi structure
extern struct uwsgi_server uwsgi;

// this is the global configuration
static struct uwsgi_consul {
	// this is the list of registered services
	struct uwsgi_string_list *services;

	// if 1, suspend the threads
	int deregistering;
} uconsul;

// this memory structure is allocated for each service
struct uwsgi_consul_service {
	CURL *curl;
	char *url;
	char *deregister_url;
	char *register_url;
	char *check_url;
	char *id;
	char *name;
	char *port;
	char *tags;
	char *ttl_string;
	int ttl;
	char *ssl_no_verify;
	char *debug;
	char *wait_workers_string;
	int wait_workers;
	// this buffer holds the pre-generated json
	struct uwsgi_buffer *ub;
};

static struct uwsgi_option consul_options[] = {
	{"consul-register", required_argument, 0, "register the specified service in a consul agent", uwsgi_opt_add_string_list, &uconsul.services, UWSGI_OPT_MASTER},
	UWSGI_END_OF_OPTIONS
};

static size_t consul_debug(void *ptr, size_t size, size_t nmemb, void *data) {
	uwsgi_log("%.*s", size*nmemb, ptr);
	return size*nmemb;
}

static void consul_loop(struct uwsgi_thread *ut) {
	struct uwsgi_consul_service *ucs = (struct uwsgi_consul_service *) ut->data;
	uwsgi_log("[consul] thread for register_url=%s check_url=%s name=%s id=%s started\n", ucs->register_url, ucs->check_url, ucs->name, ucs->id);
	if (ucs->wait_workers > 0 && uwsgi.numproc > 0) {
		uwsgi_log_verbose("[consul] waiting for workers before registering service ...\n");
		for(;;) {
			int ready = 1;
			int i;
			for(i=1;i<=uwsgi.numproc;i++) {
				if (!uwsgi.workers[i].accepting) {
					ready = 0;
					break;
				}
			}

			if (!ready) {
				sleep(1);
				continue;
			}

			uwsgi_log_verbose("[consul] workers ready, let's register the service to the agent\n");
			break;
		}
	}
	for(;;) {
		if (uconsul.deregistering) return;
		// initialize curl for the service
		ucs->curl = curl_easy_init();
		if (!ucs->curl) {
			uwsgi_log("[consul] unable to initialize curl\n");
			goto next;
		}
		struct curl_slist *headers = NULL;
		headers = curl_slist_append(headers, "Content-Type: application/json");
		curl_easy_setopt(ucs->curl, CURLOPT_TIMEOUT, ucs->ttl);
		curl_easy_setopt(ucs->curl, CURLOPT_CONNECTTIMEOUT, ucs->ttl);
		curl_easy_setopt(ucs->curl, CURLOPT_HTTPHEADER, headers); 
		curl_easy_setopt(ucs->curl, CURLOPT_URL, ucs->register_url);
		curl_easy_setopt(ucs->curl, CURLOPT_CUSTOMREQUEST, "PUT");
		curl_easy_setopt(ucs->curl, CURLOPT_POSTFIELDS, ucs->ub->buf);
		if (ucs->ssl_no_verify) {
			curl_easy_setopt(ucs->curl, CURLOPT_SSL_VERIFYPEER, 0L);
			curl_easy_setopt(ucs->curl, CURLOPT_SSL_VERIFYHOST, 0L);
		}
		if (ucs->debug) {
			curl_easy_setopt(ucs->curl, CURLOPT_HEADER, 1L);
			curl_easy_setopt(ucs->curl, CURLOPT_WRITEFUNCTION, consul_debug);
		}
		CURLcode res = curl_easy_perform(ucs->curl);	
		curl_slist_free_all(headers);
		if (res != CURLE_OK) {
			uwsgi_log("[consul] error sending request to %s: %s\n", ucs->register_url, curl_easy_strerror(res));	
			curl_easy_cleanup(ucs->curl);
			goto next;
		}
		else {
			long http_code = 0;
#ifdef CURLINFO_RESPONSE_CODE
			curl_easy_getinfo(ucs->curl, CURLINFO_RESPONSE_CODE, &http_code);
#else
			curl_easy_getinfo(ucs->curl, CURLINFO_HTTP_CODE, &http_code);
#endif
			if (http_code != 200) {
				uwsgi_log("[consul] HTTP api returned non-200 response code: %d\n", (int) http_code);
				curl_easy_cleanup(ucs->curl);
				goto next;
			}
		}
		curl_easy_cleanup(ucs->curl);

		for(;;) {
			if (uconsul.deregistering) return;
			// now call the pass check api
			// initialize curl for the service check
			ucs->curl = curl_easy_init();
			if (!ucs->curl) {
				uwsgi_log("[consul] unable to initialize curl\n");
				break;
			}
			curl_easy_setopt(ucs->curl, CURLOPT_TIMEOUT, ucs->ttl);
			curl_easy_setopt(ucs->curl, CURLOPT_CONNECTTIMEOUT, ucs->ttl);
			curl_easy_setopt(ucs->curl, CURLOPT_URL, ucs->check_url);
			if (ucs->ssl_no_verify) {
				curl_easy_setopt(ucs->curl, CURLOPT_SSL_VERIFYPEER, 0L);
				curl_easy_setopt(ucs->curl, CURLOPT_SSL_VERIFYHOST, 0L);
			}
			if (ucs->debug) {
				curl_easy_setopt(ucs->curl, CURLOPT_WRITEFUNCTION, consul_debug);
				curl_easy_setopt(ucs->curl, CURLOPT_HEADER, 1L);
			}
			res = curl_easy_perform(ucs->curl);
			if (res != CURLE_OK) {
				uwsgi_log("[consul] error sending request to %s: %s\n", ucs->check_url, curl_easy_strerror(res));
				curl_easy_cleanup(ucs->curl);
				break;
			}
			else {
				long http_code = 0;
#ifdef CURLINFO_RESPONSE_CODE
				curl_easy_getinfo(ucs->curl, CURLINFO_RESPONSE_CODE, &http_code);
#else
				curl_easy_getinfo(ucs->curl, CURLINFO_HTTP_CODE, &http_code);
#endif
				if (http_code != 200) {
					uwsgi_log("[consul] HTTP api returned non-200 response code: %d\n", (int) http_code);
					curl_easy_cleanup(ucs->curl);
					break;
				}
			}
			curl_easy_cleanup(ucs->curl);
			if (uconsul.deregistering) return;
			// wait for the ttl / 3
			sleep(ucs->ttl / 3);
		}

next:
	if (uconsul.deregistering) return;
		sleep(ucs->ttl);
	}
}

static void consul_deregister(struct uwsgi_consul_service *ucs) {
	ucs->curl = curl_easy_init();
	if (!ucs->curl) {
		uwsgi_log("[consul] unable to initialize curl\n");
		return;
	}
	curl_easy_setopt(ucs->curl, CURLOPT_TIMEOUT, ucs->ttl);
	curl_easy_setopt(ucs->curl, CURLOPT_CONNECTTIMEOUT, ucs->ttl);
	curl_easy_setopt(ucs->curl, CURLOPT_URL, ucs->deregister_url);
	if (ucs->ssl_no_verify) {
		curl_easy_setopt(ucs->curl, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(ucs->curl, CURLOPT_SSL_VERIFYHOST, 0L);
	}
	if (ucs->debug) {
		curl_easy_setopt(ucs->curl, CURLOPT_WRITEFUNCTION, consul_debug);
		curl_easy_setopt(ucs->curl, CURLOPT_HEADER, 1L);
	}
	CURLcode res = curl_easy_perform(ucs->curl);
	if (res != CURLE_OK) {
		uwsgi_log("[consul] error sending request to %s: %s\n", ucs->deregister_url, curl_easy_strerror(res));
	}
	curl_easy_cleanup(ucs->curl);
}

static void consul_setup() {
	// check sanity of requested services and 
	// create the uwsgi_consul_service structures.
	// each structure will generate a thread sending healthchecks
	// in background at the specified frequency (ttl)
	struct uwsgi_string_list *usl = NULL;
	uwsgi_foreach(usl, uconsul.services) {
		struct uwsgi_consul_service *ucs = uwsgi_calloc(sizeof(struct uwsgi_consul_service));
		usl->custom_ptr = ucs;
		if (uwsgi_kvlist_parse(usl->value, usl->len, ',', '=',
			"url", &ucs->url,
			"register_url", &ucs->register_url,
			"deregister_url", &ucs->deregister_url,
			"check_url", &ucs->check_url,
			"id", &ucs->id,
			"name", &ucs->name,
			"port", &ucs->port,
			"tags", &ucs->tags,
			"ttl", &ucs->ttl_string,
			"ssl_no_verify", &ucs->ssl_no_verify,
			"debug", &ucs->debug,
			"wait_workers", &ucs->wait_workers_string,
		NULL)) {
			uwsgi_log("[consul] unable to parse service: %s\n", usl->value);
			exit(1);
		}

		if (!ucs->name) {
			uwsgi_log("[consul] name is required: %s\n", usl->value);
			exit(1);
		}

		if (!ucs->id) {
			ucs->id = ucs->name;
		}

		if (!ucs->register_url) {
			if (!ucs->url) {
				uwsgi_log("[consul] url or register_url is required: %s\n", usl->value);
				exit(1);
			}
			ucs->register_url = uwsgi_concat2(ucs->url, "/v1/agent/service/register");
		}

		if (!ucs->check_url) {
			if (!ucs->url) {
				uwsgi_log("[consul] url or check_url is required: %s\n", usl->value);
				exit(1);
			}
			ucs->check_url = uwsgi_concat3(ucs->url, "/v1/agent/check/pass/service:", ucs->id);
		}

		if (!ucs->deregister_url) {
			if (!ucs->url) {
				uwsgi_log("[consul] url or deregister_url is required: %s\n", usl->value);
				exit(1);
			}
			ucs->deregister_url = uwsgi_concat3(ucs->url, "/v1/agent/service/deregister/", ucs->id);
		}

		// convert TTL to integer and default to 30 seconds if not set
		if (ucs->ttl_string) ucs->ttl = atoi(ucs->ttl_string);
		if (!ucs->ttl) ucs->ttl = 30;

		// wait_workers defaults to 1
		if (ucs->wait_workers_string) {
			ucs->wait_workers = atoi(ucs->wait_workers_string);
		}
		else {
			ucs->wait_workers = 1;
		}

		// pre-generate the JSON
		// {"Name":"xxx","ID":"xxx","Check":{"TTL": "xxxs"},"Port":xxx,"Tags":["xxx",...]}
		ucs->ub = uwsgi_buffer_new(uwsgi.page_size);
		if (uwsgi_buffer_append(ucs->ub, "{\"Name\":\"", 9)) goto error;
		if (uwsgi_buffer_append_json(ucs->ub, ucs->name, strlen(ucs->name))) goto error;

		if (uwsgi_buffer_append(ucs->ub, "\",\"ID\":\"", 8)) goto error;
		if (uwsgi_buffer_append_json(ucs->ub, ucs->id, strlen(ucs->id))) goto error;

		if (uwsgi_buffer_append(ucs->ub, "\",\"Check\":{\"TTL\":\"", 18)) goto error;
		if (uwsgi_buffer_num64(ucs->ub, ucs->ttl)) goto error;
		if (uwsgi_buffer_append(ucs->ub, "s\"}", 3)) goto error;

		// port ?
		if (ucs->port) {
			if (uwsgi_buffer_append(ucs->ub, ",\"Port\":", 8)) goto error;
			if (uwsgi_buffer_num64(ucs->ub, atoi(ucs->port))) goto error;
		}

		// tags ?
		if (ucs->tags) {
			char *tags = uwsgi_str(ucs->tags);
			if (uwsgi_buffer_append(ucs->ub, ",\"Tags\":[", 9)) goto error;
			char *p, *ctx = NULL;
			int has_tags = 0;
			uwsgi_foreach_token(tags, " ", p, ctx) {
				has_tags++;
				if (uwsgi_buffer_append(ucs->ub, "\"", 1)) goto error;
				if (uwsgi_buffer_append_json(ucs->ub, p, strlen(p))) goto error;
				if (uwsgi_buffer_append(ucs->ub, "\",", 2)) goto error;
			}
			// remove the last comma (if required)
			if (has_tags) ucs->ub->pos--;
			if (uwsgi_buffer_append(ucs->ub, "]", 1)) goto error;
			free(tags);
		}

		// final zero is required for curl
		if (uwsgi_buffer_append(ucs->ub, "}\0", 2)) goto error;

		uwsgi_log("[consul] built service JSON: %.*s\n", ucs->ub->pos, ucs->ub->buf);

		// deregister the service (ignoring errors)
		consul_deregister(ucs);

		// let's spawn the thread
		uwsgi_thread_new_with_data(consul_loop, ucs);
	}
	return;
error:
	uwsgi_log("[consul] unable to generate JSON\n");
	exit(1);
}

static void consul_deregister_all() {
	// this will end threads
	uconsul.deregistering = 1;

	struct uwsgi_string_list *usl;
	uwsgi_foreach(usl, uconsul.services) {
		struct uwsgi_consul_service *ucs = (struct uwsgi_consul_service *) usl->custom_ptr;
		uwsgi_log("[consul] deregistering %s\n", ucs->deregister_url);
		consul_deregister(ucs);
	}
}

struct uwsgi_plugin consul_plugin = {
	.name = "consul",
	.options = consul_options,
	.postinit_apps = consul_setup,
	.master_cleanup = consul_deregister_all,
};
