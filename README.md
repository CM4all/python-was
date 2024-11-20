# python-was

`python-was` is a [WAS](https://github.com/CM4all/libwas)-server that can serve Python (currently only WSGI) applications.

# Building

```
# Configure
git submodule update --init libcommon/
meson setup build

# Build
ninja -C build
```

# Usage

Like other WSGI servers `python-was` needs to know where it can find the application object to be used as a WSGI application.
To specify it, you should use the `--sys-path <path>` flag, which allows you to add module search paths, the `--module <name>` flag, which specifies the name of the module that contains the WSGI application and `--app <name>` which specifies the name of the object that should be used as a WSGI application.
Make sure you specify `--sys-path` not just for your application itself, but also for it's dependencies (like Flask or Django).

Example:

```
build/python-was --sys-path ./testapp --sys-path .venv/lib/python3.11/site-packages/ --module hello --app app
```

Note that this is not how `python-was` is used in practice, but a simple test mode that processes a few hard coded requests, when stdin is a TTY.

If stdin is not a TTY, `python-was` will assume it is executed by a WAS client.

[`beng-proxy`](https://github.com/CM4all/beng-proxy) includes a test program called `run_was`, which is a very simple WAS client:

```shell
../beng-proxy/build/test/run_was build/python-was / -- \
	--sys-path ./testapp --sys-path .venv/lib/python3.11/site-packages/ --module hello --app app
```

# Notes

If run on stretch, Python code is not automatically reloaded, so you have to run either `cm4all-beng-control fade-children` (as root) or `apachectl reload` (in your webspace) after you have modified it to trigger a restart of `python-was`.

# ToDo

- Test "Transfer-Encoding: chunked" for both request and response.
- Make parameters like module and app request parameters instead of CLI parameters. This would require multiple Python interpereters.
- Dedicated API to set app object explicitly, e.g.: `import cm4all_python_was; cm4all_python_was.set_app(appl, config_foo=True)`.
- Find common interface for WSGI and ASGI.
- Implement missing `WsgiInputStream` methods.
- Sending files: `wsgi.file_wrapper`, `X-SendFile`?
- ASGI
- Async WAS client instead of `was_simple` - doesn't matter now, because WSGI cannot do concurrent requests anyways.
- Multi-Was / Mutliple Processes - beng-proxy can start multiple WAS processes anyways, so the only benefit would be that we could load the module and then fork, which might improve start times.
- Multi-Threading - you can create multiple interpreters, but they share a GIL, so it doesn't help much.
- Distribution: Python wheels do not include dependencies, there is no canonical way to distribute a package including dependencies. Usually it's only a list of requirements (either in the wheel metadata or as requirements.txt). Alternatively you can upload a whole venv (tricky with the binaries) or a number of wheels. Or provide a package manager.
- write()-callable returned from start_response - deprecated and should not be used by applications, also generally bad, but might be required by old applications, so it is a "MUST".
- Range-Requests.
- Python 2.
- Find a way to reload Python files when they are modified.
- Benchmark against [Litespeed](https://openlitespeed.org/) and [Granian](https://github.com/emmett-framework/granian).
