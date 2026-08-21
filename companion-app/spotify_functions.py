import asyncio
from winrt.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionManager as MediaManager,
    GlobalSystemMediaTransportControlsSessionPlaybackStatus as PlaybackStatus,
)
import datetime

async def function_find_spotify():
    manager = await MediaManager.request_async()
    for session in manager.get_sessions():
        app_id = session.source_app_user_model_id.lower()
        if "spotify" in app_id:
            return session
    return None

def function_get_time_position(session): # not async since it is instant
    timeline = session.get_timeline_properties()
    
    now = datetime.datetime.now(datetime.timezone.utc) # work with UTC to make sure everything matches in all regions
    elapsed = (now - timeline.last_updated_time).total_seconds() # returns a UTC therefore work with UTC
    
    reported = timeline.position.total_seconds()

    info = session.get_playback_info()
    if info.playback_status == PlaybackStatus.PLAYING: # if music is playing reported + elapsed works but if it isn't you run into the issue of reported being accurate + elapsed
        corrected = reported + elapsed
    else:
        corrected = reported

    return corrected

async def function_set_shuffle(session, shuffle_request):
    return await session.try_change_shuffle_active_async(shuffle_request)

async def function_toggle_shuffle(session): # change a shuffle on into off and vice versa
    info = session.get_playback_info()
    return await function_set_shuffle(session, not info.is_shuffle_active) # swap em around with set shuffle function

async def function_shuffle_wrapper(session, params=None):
    if params is None: return await function_toggle_shuffle(session)
    elif params.upper() == "ON": return await function_set_shuffle(session, True)
    elif params.upper() == "OFF": return await function_set_shuffle(session, False)
    else: 
        print(f'Cannot find param: {params} for shuffle')
        return

# this function is called by function_seek_wrapper_forward and function_seek_wrapper_backward. This is because of the negative
# floats being a possible issue in self made configs
async def function_try_seek(session, offset_seconds):
    position = function_get_time_position(session)
    target = position + offset_seconds

    print(f"seeking to target: {target:2f}")

    ticks = int(target * 10_000_000)
    return await session.try_change_playback_position_async(ticks)

async def function_seek_wrapper_forward(session, params=None):
    try: # guard test to see if param can indeed be a float.
        offset = float(params)
    except(ValueError, TypeError):
        return print(f'current param: {params} does not work. For seek you need a number in seconds. e.g 10.5 or 4')
    return await function_try_seek(session, float(offset))

async def function_seek_wrapper_backward(session, params=None):
    try: # guard test to see if param can indeed be a float.
        offset = float(params)
    except(ValueError, TypeError):
        return print(f'current param: {params} does not work. For seek you need a number in seconds. e.g 10.5 or 4')

    return await function_try_seek(session, -float(offset))

async def function_show_now_playing(session, params = None):

    props = await session.try_get_media_properties_async()

    metadata = {
        "title": props.title,
        "artist": props.artist,
        "album_title": props.album_title,
        "album_artist": props.album_artist,
        "subtitle": props.subtitle,
        "track_number": props.track_number,
        "genres": list(props.genres),
    }
    return metadata

async def function_play_pause(session, params = None):
    return await session.try_toggle_play_pause_async()

async def function_skip_next_song(session, params = None):
    return await session.try_skip_next_async()

async def function_previous_song(session, params = None):
    return await session.try_skip_previous_async()
    

# if you have add a new function to spotify SMTC. Make sure to add it here so the function handler can find it
# the first part must match the action line coming from serial monitor that is read by main.py
SPOTIFY_FUNCTIONS = {
    # "GETTIME": function_get_time_position,
    "SHUFFLE": function_shuffle_wrapper,
    "SEEKFWD": function_seek_wrapper_forward,
    "SEEKBACK": function_seek_wrapper_backward,
    "SHOWDATA": function_show_now_playing,
    "PLAYPAUSE": function_play_pause,
    "NEXT": function_skip_next_song,
    "PREV": function_previous_song
}

async def function_handle_spotify_functions(action):
    params = None
    if ":" in action: # an action would only contain : at this point if it holds params
        action, params = action.split(":", 1)

    if action not in SPOTIFY_FUNCTIONS:
        print(f"action not recognised: {action}")
        return

    session = await function_find_spotify()
    if session is None:
        print("Spotify is not running")
        return

    func = SPOTIFY_FUNCTIONS[action]
    await func(session, params)
    