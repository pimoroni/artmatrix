import uasyncio
import os
import sys
import time
from . import logging

# Bytes per read when streaming a file. Each chunk costs a drain, and the draw
# task can hold the loop for tens of milliseconds between them.
FILE_CHUNK_SIZE = 4096

_routes = []
catchall_handler = None
loop = uasyncio.get_event_loop()


def file_exists(filename):
  try:
    return (os.stat(filename)[0] & 0x4000) == 0
  except OSError:
    return False


def urldecode(text):
  """Decode url encoding, tolerating malformed input.

  Non-hex characters after a % are passed through literally, and byte sequences
  that are not valid utf-8 become the unicode replacement character."""
  text = text.replace("+", " ")
  result = []
  token_caret = 0
  # decode any % encoded characters
  while True:
    start = text.find("%", token_caret)
    if start == -1:
      result += text[token_caret:]
      break
    result += text[token_caret:start]
    encoded = bytearray()
    # a utf-8 character can span several % escapes, so gather the whole run
    while len(text) > start and text[start] == "%":
      try:
        encoded.append(int(text[start + 1:start + 3], 16))
      except ValueError:
        result += _decode_or_replace(encoded)
        # not an encoded byte, so pass the text through as-is
        result += text[start:start + 3]
        encoded = bytearray()
      token_caret = start + 3
      start = start + 3
    result += _decode_or_replace(encoded)
  return "".join(result)


def _decode_or_replace(encoded):
  try:
    return encoded.decode()
  except (UnicodeError, UnicodeDecodeError):
    return "\ufffd"


def _parse_query_string(query_string):
  result = {}
  for parameter in query_string.split("&"):
    if not parameter:
      continue
    # a parameter with no "=" is a key with an empty value
    key, _, value = parameter.partition("=")
    result[urldecode(key)] = urldecode(value)
  return result


class Request:
  def __init__(self, method, uri, protocol):
    self.method = method
    self.uri = uri
    self.protocol = protocol
    self.form = {}
    self.data = {}
    self.query = {}
    query_string_start = uri.find("?") if uri.find("?") != -1 else len(uri)
    self.path = uri[:query_string_start]
    self.query_string = uri[query_string_start + 1:]
    if self.query_string:
      self.query = _parse_query_string(self.query_string)

  def __str__(self):
    return f"""\
request: {self.method} {self.path} {self.protocol}
headers: {self.headers}
form: {self.form}
data: {self.data}"""


class Response:
  def __init__(self, body, status=200, headers=None):
    self.status = status
    self.headers = headers or {}
    self.body = body

  def add_header(self, name, value):
    self.headers[name] = value

  def __str__(self):
    return f"""\
status: {self.status}
headers: {self.headers}
body: {self.body}"""


content_type_map = {
  "html": "text/html",
  "jpg": "image/jpeg",
  "jpeg": "image/jpeg",
  "svg": "image/svg+xml",
  "json": "application/json",
  "png": "image/png",
  "css": "text/css",
  "js": "text/javascript",
  "csv": "text/csv",
}


class FileResponse(Response):
  def __init__(self, file, status=404, headers=None):
    self.status = status
    self.headers = headers or {}
    self.file = file

    try:
      stat = os.stat(self.file)
      if (stat[0] & 0x4000) == 0:
        self.status = 200

        # auto set content type
        extension = self.file.split(".")[-1].lower()
        if extension in content_type_map:
          self.headers["Content-Type"] = content_type_map[extension]

        self.headers["Content-Length"] = stat[6]

        # Size and mtime as an opaque validator, so a client can revalidate with
        # If-None-Match and take a 304 instead of refetching the whole file.
        # An ETag avoids formatting an HTTP date, which MicroPython's 2000-based
        # epoch makes awkward to get right.
        self.headers["ETag"] = '"{0}-{1}"'.format(stat[6], stat[8])
    except OSError:
      pass


class Route:
  def __init__(self, path, handler, methods=None):
    self.path = path
    self.methods = methods or ["GET"]
    self.handler = handler
    self.path_parts = path.split("/")

  # returns True if the supplied request matches this route
  def matches(self, request):
    if request.method not in self.methods:
      return False
    compare_parts = request.path.split("/")
    if len(compare_parts) != len(self.path_parts):
      return False
    for part, compare in zip(self.path_parts, compare_parts):
      if not part.startswith("<") and part != compare:
        return False
    return True

  # call the route handler passing any named parameters in the path
  def call_handler(self, request):
    parameters = {}
    for part, compare in zip(self.path_parts, request.path.split("/")):
      if part.startswith("<"):
        name = part[1:-1]
        parameters[name] = compare

    return self.handler(request, **parameters)

  def __str__(self):
    return f"""\
path: {self.path}
methods: {self.methods}
"""

  def __repr__(self):
    return f"<Route object {self.path} ({', '.join(self.methods)})>"


# parses the headers for a http request (or the headers attached to
# each field in a multipart/form-data)
async def _parse_headers(reader):
  headers = {}
  while True:
    header_line = await reader.readline()
    if header_line == b"\r\n": # crlf denotes body start
      break
    name, value = header_line.decode().strip().split(": ", 1)
    headers[name.lower()] = value
  return headers


# returns the route matching the supplied path or None
def _match_route(request):
  for route in _routes:
    if route.matches(request):
      return route
  return None


# if the content type is multipart/form-data then parse the fields
async def _parse_form_data(reader, headers):
  boundary = headers["content-type"].split("boundary=")[1]
  # discard first boundary line
  _dummy = await reader.readline()

  delimiter = b"--" + boundary.encode()
  terminator = delimiter + b"--"

  form = {}
  while True:
    # get the field name
    field_headers = await _parse_headers(reader)
    if len(field_headers) == 0:
      break
    name = field_headers["content-disposition"].split('name="')[1][:-1]
    # get the field value
    lines = []
    while True:
      line = await reader.readline()
      if not line:
        return form
      boundary_line = line.rstrip(b"\r\n")
      if boundary_line == delimiter or boundary_line == terminator:
        value = b"".join(lines)
        if value.endswith(b"\r\n"):
          value = value[:-2]
        form[name] = value.decode()
        if boundary_line == terminator:
          return form
        break
      lines.append(line)
  return None


# if the content type is application/json then parse the body
async def _parse_json_body(reader, headers):
  import json
  content_length_bytes = int(headers["content-length"])
  body = await reader.readexactly(content_length_bytes)
  return json.loads(body.decode())


_write_chunk_size = 1024

status_message_map = {
  200: "OK", 201: "Created", 202: "Accepted",
  203: "Non-Authoritative Information", 204: "No Content",
  205: "Reset Content", 206: "Partial Content", 300: "Multiple Choices",
  301: "Moved Permanently", 302: "Found", 303: "See Other",
  304: "Not Modified", 305: "Use Proxy", 306: "Switch Proxy",
  307: "Temporary Redirect", 308: "Permanent Redirect",
  400: "Bad Request", 401: "Unauthorized", 403: "Forbidden",
  404: "Not Found", 405: "Method Not Allowed", 406: "Not Acceptable",
  408: "Request Timeout", 409: "Conflict", 410: "Gone",
  414: "URI Too Long", 415: "Unsupported Media Type",
  416: "Range Not Satisfiable", 418: "I'm a teapot",
  500: "Internal Server Error", 501: "Not Implemented"
}


# handle an incoming request to the web server
async def _handle_request(reader, writer):
  response = None

  request_start_time = time.ticks_ms()

  request_line = await reader.readline()
  try:
    method, uri, protocol = request_line.decode().split()
  except Exception as e:  # noqa: BLE001
    logging.error(e)
    writer.close()
    await writer.wait_closed()
    return

  request = Request(method, uri, protocol)
  request.headers = await _parse_headers(reader)
  if "content-length" in request.headers and "content-type" in request.headers:
    if request.headers["content-type"].startswith("multipart/form-data"):
      request.form = await _parse_form_data(reader, request.headers)
    if request.headers["content-type"].startswith("application/json"):
      request.data = await _parse_json_body(reader, request.headers)
    if request.headers["content-type"].startswith("application/x-www-form-urlencoded"):
      remaining = int(request.headers["content-length"])
      chunks = []
      while remaining > 0:
        data = await reader.read(remaining)
        if not data:
          break
        chunks.append(data)
        remaining -= len(data)
      form_data = chunks[0] if len(chunks) == 1 else b"".join(chunks)
      request.form = _parse_query_string(form_data.decode())

  # A handler raising would otherwise kill this task with the socket still open,
  # leaving the client to hang until it times out.
  try:
    route = _match_route(request)
    if route:
      response = route.call_handler(request)
    elif catchall_handler:
      response = catchall_handler(request)
    else:
      response = ("Not Found", 404, "text/plain")
  except Exception as e:  # noqa: BLE001
    logging.exception(f"! {request.method} {request.path} raised {e}")
    sys.print_exception(e)
    response = ("500 Internal Server Error", 500, "text/plain")

  # a handler that returns nothing would otherwise take the connection down with
  # it, leaving the socket open and the client waiting
  if response is None:
    logging.error(f"! no response returned for {request.method} {request.path}")
    response = ("Internal Server Error", 500, "text/plain")

  # if shorthand body generator only notation used then convert to tuple
  if type(response).__name__ == "generator":
    response = (response,)

  # if shorthand body text only notation used then convert to tuple
  if isinstance(response, str):
    response = (response,)

  # if shorthand tuple notation used then build full response object
  if isinstance(response, tuple):
    body = response[0]
    status = response[1] if len(response) >= 2 else 200
    content_type = response[2] if len(response) >= 3 else "text/html"
    response = Response(body, status=status)
    response.add_header("Content-Type", content_type)
    if hasattr(body, "__len__"):
      response.add_header("Content-Length", len(body))

  # the client already holds this file, so send the validator back and no body
  send_body = True
  if isinstance(response, FileResponse) and response.status == 200:
    if request.headers.get("if-none-match") == response.headers.get("ETag"):
      response.status = 304
      response.headers.pop("Content-Length", None)
      send_body = False

  # there is no file to stream
  if isinstance(response, FileResponse) and response.status != 200:
    send_body = False

  # the socket closes below, so say so rather than let the client attempt to
  # reuse the connection and hit the FIN
  response.headers["Connection"] = "close"

  # write status line
  status_message = status_message_map.get(response.status, "Unknown")
  writer.write(f"HTTP/1.1 {response.status} {status_message}\r\n".encode("ascii"))

  # write headers
  for key, value in response.headers.items():
    writer.write(f"{key}: {value}\r\n".encode("ascii"))

  # blank line to denote end of headers
  writer.write("\r\n".encode("ascii"))

  if not send_body:
    await writer.drain()
  elif isinstance(response, FileResponse):
    # file
    buffer = bytearray(FILE_CHUNK_SIZE)
    buffer_view = memoryview(buffer)
    with open(response.file, "rb") as f:
      while True:
        count = f.readinto(buffer)
        if not count:
          break
        writer.write(buffer_view[:count])
        await writer.drain()
  # text goes out as bytes. the stream slices off what a write sent by its byte
  # count, so a str holding a multi-byte character loses characters when the
  # socket takes only part of it
  elif type(response.body).__name__ == "generator":
    # generator
    pending = []
    pending_size = 0
    for chunk in response.body:
      if isinstance(chunk, str):
        chunk = chunk.encode()
      pending.append(chunk)
      pending_size += len(chunk)
      if pending_size >= _write_chunk_size:
        writer.write(b"".join(pending))
        await writer.drain()
        pending = []
        pending_size = 0
    if pending:
      writer.write(b"".join(pending))
      await writer.drain()
  else:
    # string/bytes
    body = response.body
    writer.write(body.encode() if isinstance(body, str) else body)
    await writer.drain()

  writer.close()
  await writer.wait_closed()

  processing_time = time.ticks_ms() - request_start_time
  logging.info(f"> {request.method} {request.path} ({response.status} {status_message}) [{processing_time}ms]")


# adds a new route to the routing table
def add_route(path, handler, methods=None):
  global _routes
  methods = methods or ["GET"]
  _routes.append(Route(path, handler, methods))
  # descending complexity order so most complex routes matched first
  _routes = sorted(_routes, key=lambda route: len(route.path_parts), reverse=True)


def clear_routes():
  global _routes, catchall_handler
  _routes = []
  catchall_handler = None


def set_callback(handler):
  global catchall_handler
  catchall_handler = handler


# decorator shorthand for adding a route
def route(path, methods=None):
  methods = methods or ["GET"]
  def _route(f):
    add_route(path, f, methods=methods)
    return f
  return _route


# decorator for adding catchall route
def catchall():
  def _catchall(f):
    set_callback(f)
    return f
  return _catchall


def redirect(url, status = 301):
  return Response("", status, {"Location": url})


def serve_file(file):
  return FileResponse(file)


def run(host = "0.0.0.0", port = 80):
  logging.info("> starting web server on port {}".format(port))
  loop.create_task(uasyncio.start_server(_handle_request, host, port))
  loop.run_forever()

def stop():
  loop.stop()

def close():
  loop.close()
