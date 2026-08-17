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


    print(f"reported:  {reported:.2f}")
    print(f"elapsed:   {elapsed:.2f}")
    print(f"corrected: {corrected:.2f}")
    return corrected

async def function_try_seek(session, offset_seconds):
    position = function_get_time_position(session)
    target = position + offset_seconds

    print(f"seeking to target: {target:2f}")

    ticks = int(target * 10_000_000)
    return await session.try_change_playback_position_async(ticks)

async def function_show_now_playing():
    session = await function_find_spotify()
    if session is None:
        print("Spotify is not running")
        return

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

    for name, value in metadata.items():
        print(f"{name:14}{value}")

    # --- test 2: getting exact times ---
    print(function_get_time_position(session))


session = asyncio.run(function_find_spotify())
asyncio.run(function_try_seek(session, -200))
# asyncio.run(function_show_now_playing())