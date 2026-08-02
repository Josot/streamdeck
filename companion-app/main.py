import serial # used to read out serial
import subprocess # Used for shell catagory
from pynput.keyboard import Controller, Key # used to simulate keyboard presses

keyboard = Controller()

baudRate = 115200
comPort = "COM13"

connection = serial.Serial(comPort, baudRate, timeout=1)

# maps the key names to pynputs key objects. Single characters (c, a, 4, $) are absent pynput accepts those as plain strings.
# add an entry here when when a new keybinds uses a key that is not included in KEY_NAMES yet.
KEY_NAMES = {
    "CTRL": Key.ctrl,
    "SHIFT": Key.shift,
    "ALT": Key.alt,
    "WIN": Key.cmd, # pynput is cross platform so windows key is CMD here like mac

    # MEDIA KEYS
    "MEDIA_PLAYPAUSE": Key.media_play_pause,
    "MEDIA_NEXT": Key.media_next,
    "MEDIA_PREV": Key.media_previous,
    "MEDIA_VOLUP": Key.media_volume_up,
    "MEDIA_VOLDOWN": Key.media_volume_down,
    "MEDIA_MUTE": Key.media_volume_mute,
}

def function_resolve_keys(key):
    if key in KEY_NAMES:
        return KEY_NAMES[key]
    elif len(key) == 1:
        return key.lower()
    else:
        return None

def function_execute_keybinds(resolvedKeys):
    # seperated in first keys and the last key due the way keybinds work, they always need
    # the first few keys held and then the last one 'tapped' once to register it.
    heldKeys = resolvedKeys[:-1] # Everything except the last resolved key
    tappedKey = resolvedKeys[-1] # Merely the last resolved key
    
    for key in heldKeys:
        keyboard.press(key)
    
    keyboard.press(tappedKey)
    keyboard.release(tappedKey)
    
    for key in reversed(heldKeys):
        keyboard.release(key)

while True:

    # Read the line and save it it for futher use
    line = connection.readline() # read the serial print on baudrate set in connection
    line = line.decode("utf-8", errors="replace").strip() # strip away /r/n
    if not line: # it prints '' in between keystrokes due to timeout from serial connection which is false therefore continue to not flood terminal.
        continue

    print(line)

    if ":" not in line: # if there is no ":" in the line only 1 item will exist and it will fail split category action
        continue

    category, action = line.split(":", 1) # 1 cut due to some names being example: OBS:SCENE:1 or SHELL:C:\Windows\notepad.exe


    if category == "KEY":
        keys = action.split("+") # split it into keys for example WIN+SHIFT+S becomes [0] WIN [1] SHIFT [2] S
        resolvedKeys = [] # put in all keys that went through function_resolve_keys

        for key in keys:
            key = function_resolve_keys(key)
            resolvedKeys.append(key)

        if None in resolvedKeys: # if function_resolve_keys couldn't resolve the given 
            print(f"Unknown key in: {line}")
            continue

        function_execute_keybinds(resolvedKeys)

    elif category == "SHELL":
        subprocess.Popen(action, shell=True) # perform action, use Popen because it is NOT blocking

    else:
        print(f"cannot find the given category: {category}")