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
"""
