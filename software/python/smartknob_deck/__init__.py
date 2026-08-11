"""
Let a SmartKnob control the computer it's plugged into (volume, brightness).

The knob owns its menu, haptics and display; this agent follows it. The firmware
sends plaintext lines over the same USB serial port it already logs to:

    @SET <channel> <value>   knob was turned -> apply it here
    @GET <channel>           knob opened a page -> answer with the OS value

and the agent replies (also when something else changes a value):

    @VAL <channel> <value>

Run it with:
    py -m smartknob_deck

Install it to start hidden at login with:
    py -m smartknob_deck.install_autostart

The agent keeps the serial port open, and nothing else can share it. Uploads
handle that themselves (scripts/deck_upload_hook.py stops the agent and starts it
again), but anything else that wants the port - `pio device monitor` above all -
needs it out of the way first:

    py -m smartknob_deck --stop      free the port
    py -m smartknob_deck --restart   put the agent back
"""
