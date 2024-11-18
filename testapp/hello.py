from flask import Flask, request

app = Flask(__name__)

@app.route("/")
def hello_world():
    return "<p>Hello, World!</p>"

@app.route("/", methods=["PUT"])
def put():
    return request.json

@app.route("/error")
def error():
    raise IOError("Intentional error")

@app.route("/empty")
def empty():
    return ""
