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
