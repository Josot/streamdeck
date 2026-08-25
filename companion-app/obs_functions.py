import obsws_python as obs
import time

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

def function_set_scene(client, params=None):
    client.set_current_program_scene(params)

def function_stream_toggle(client, params=None):
    client.toggle_stream()

def function_record_toggle(client, params=None):
    client.toggle_record()

def function_record_pause_toggle(client, params=None):
    client.toggle_record_pause()

def function_mute_toggle(client, params=None):
    client.toggle_input_mute(params)

# if you add a new OBS function, add it here so the handler can find it.
# the first part must match the action string coming from serial, read by main.py
OBS_FUNCTIONS = {
    "SCENE": function_set_scene,
    "STREAM": function_stream_toggle,
    "RECORD": function_record_toggle,
    "RECORDPAUSE": function_record_pause_toggle,
    "MUTE": function_mute_toggle,
}

def function_handle_obs_functions(action):
    params = None
    if ":" in action: # an action would only contain : at this point if it holds params
        action, params = action.split(":", 1)

    if action not in OBS_FUNCTIONS:
        print(f"action not recognised: {action}")
        return
    
    obs_client = function_get_client()
    if obs_client is None:
        print(f"skipping action: {action}. no OBS connection")
        return

    func = OBS_FUNCTIONS[action]
    func(obs_client, params)