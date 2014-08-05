uwsgi-consul
============

uWSGI plugin for consul (http://www.consul.io) integration

INSTALL
=======

The plugin is 2.x friendly:

```sh
uwsgi --build-plugin https://github.com/unbit/uwsgi-consul
```

you will end with consul_plugin.so in the current directory

USAGE
=====

The plugin exposes a single option: `consul-register`

It allows your uWSGI instance to register as a service (with TTL health ckeck) to a (preferibly local) consul agent.

The `consul-register` option is keyval based, and it takes the following keys:

* url (the api base url, generally scheme and domain, example: http://localhost:8500)
* register_url (the api url for registering the new service, if not specified is built as url+/v1/agent/service/register)
* check_url (the api url for registering the service healthcheck, if not specified is built as url+/v1/agent/check/pass/service:+id)
* id (the service instance id, required)
* name (the service name, required)
* port (the service instance port)
* tags (space separated list of tags)
* ttl (ttl for healthchecks, default 30)
* ssl_no_verify (if the http api is over https you can disable certificate verification)
* debug (print http transactions in logs, for debugging)
* wait_workers (do not register the service until all of the workers are ready)

How it works
============

A thread for each configured service is spawned in the master.

The thread registers the service with the api at the first run, then it start sending ttl checks every configured ttl/3.

On error condition, the thread restart its cycle, re-registering the service

Example
=======

```ini
[uwsgi]
plugins = python,consul
; register instance 'servicenode0002' on port 9091 for service 'foobar', waiting for workers
consul-register = url=http://localhost:8500,id=servicenode0002,name=foobar,port=9091,ttl=30,wait_workers=1
http-socket = :9091
processes = 4
wsgi-file = myapp.py
```
