import asyncio
from winrt.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionManager as MediaManager
)

async def function_find_spotify():
    manager = await MediaManager.request_async()
    for session in manager.get_sessions():
        app_id = session.source_app_user_model_id.lower()
        if "spotify" in app_id:
            print(app_id)
            return session

    return None

async def function_show_now_playing():
    session = await function_find_spotify()
    if session is None:
        print("Spotify is not running")
        return

    props = await session.try_get_media_properties_async()
    print("title:        ", props.title)
    print("artist:       ", props.artist)
    print("album_title:  ", props.album_title)
    print("album_artist: ", props.album_artist)
    print("subtitle:     ", props.subtitle)
    print("track_number: ", props.track_number)
    print("Genre:        ", props.genres)
    await session.try_skip_next_async()

    

asyncio.run(function_show_now_playing())