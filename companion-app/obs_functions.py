import obsws_python as obs

# One websocket connection, reused across presses. OBS not running is a normal
# state, not an error. a failed connect leaves this None so the next press retries
client = None

def function_get_client():
    global client # without this it would make client a local variable and raise issues on the guard below

    if client is not None:
        return client

    try:
        client = obs.ReqClient(host="localhost", port=4455, password="f8tZx1nFtwhGWCf1", timeout=3)
    except Exception as e:
        print(f"Cannot connect to OBS, is it running? {e}")

    return client

def function_set_scene(client, params):
    client.set_current_program_scene(params)

function_set_scene(function_get_client(), "End stream")