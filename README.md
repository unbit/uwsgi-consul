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
