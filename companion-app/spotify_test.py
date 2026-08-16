import asyncio
from winrt.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionManager as MediaManager
)

async def function_find_spotify():
    manager = await MediaManager.request_async()
    for session in manager.get_sessions():
        app_id = session.source_app_user_model_id.lower()
        if "spotify" in app_id:
            return session
    return None

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

    controls = session.get_playback_info().controls
    capabilities = {
        name: getattr(controls, name)
        for name in dir(controls)
        if name.startswith("is_")
    }

    print()
    for name, supported in capabilities.items():
        print(f"{name:32}{supported}")

    # --- test 1: fast forward / rewind ---
    print()
    print("fast_forward:", await session.try_fast_forward_async())
    await asyncio.sleep(2)
    print("rewind:      ", await session.try_rewind_async())

asyncio.run(function_show_now_playing())