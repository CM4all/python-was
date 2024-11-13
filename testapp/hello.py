from flask import Flask, request

app = Flask(__name__)

@app.route("/")
def hello_world():
    return "<p>Hello, World!</p>"

@app.route("/", methods=["PUT"])
def put():
    print(request.json)
    return request.json

import pprint

class DebugMiddleware(object):
    def __init__(self, flask_app):
        self._flask_app = flask_app

    def __call__(self, environ, start_response):
        pprint.pprint(environ)
        return self._flask_app(environ, start_response)

app.wsgi_app = DebugMiddleware(app.wsgi_app)
