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

def function_scene_item_visibility_toggle(client, params=None):
    # Simplified function walkthrough
    # Which scene is current? --> Which id does param (e.g Webcam) have in current scene? --> do opposite of current state of param (e.g Webcam) so turn on/off
    # fyi visibility toggle is scene specific

    scene = client.get_current_program_scene().scene_name # grab name of currently showing scene
    item_id = client.get_scene_item_id(scene, params).scene_item_id # grab id of the param (e.g Webcam) in the current scene

    current_state = client.get_scene_item_enabled(scene, item_id).scene_item_enabled # checks what the current state of the param is
    client.set_scene_item_enabled(scene, item_id, not current_state) # does the opposite of what the current param state is (like a toggle)


# if you add a new OBS function, add it here so the handler can find it.
# the first part must match the EXACT action string coming from serial, read by main.py
OBS_FUNCTIONS = {
    "SCENE": function_set_scene,
    "STREAM": function_stream_toggle,
    "RECORD": function_record_toggle,
    "RECORDPAUSE": function_record_pause_toggle,
    "MUTE": function_mute_toggle,
    "HIDETOGGLE": function_scene_item_visibility_toggle
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