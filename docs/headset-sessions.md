# Guided headset sessions

A way to test with a person in the headset while a test tool (or an AI agent) runs the session: the tool
greets them, gives short numbered steps in the game's HUD, asks questions in the game's message box, takes a
hands-up gesture as "done", and thanks them at the end. The person never has to take the headset off to read
instructions or type an answer, and many topics fit into one longer session.

It grew out of FNVVR's development, where headset time is the scarcest resource: one guided session checked a
panel's look, a readability question, a click flow and the session tools themselves in about two minutes.

## What you need

- The game channel: this plugin with `Enabled=1` (see the [README](../README.md)).
- FNVVR with its VR test channel (`d3d9.vrTestChannel = True`): the done signal and the Pip-Boy helpers use it.
  Notifications and questions alone need only the game channel.
- The Python tools in [`tools/`](../tools/) (`fnvtest`, no extra packages).
- The bridge takes one client at a time: let one script own the session and send everything through it (or
  use [`tools/watch.py`](../tools/watch.py) and its command queue next to your own tool).

## The flow

1. **Agree on the start.** The person says when they start the headset and the game. Start your script then;
   it waits for a loaded save by itself (`state` reports `world=1`), so they need not report it.
2. **Greet and arm the signal.** `headset.start(session)` turns the done signal on and shows "Raise both hands to
   signal me". The signal (both hands held above the head for about a second, counted by FNVVR's VR channel) is
   off until a tool turns it on, so it never fires while someone plays alone with the channels left on.
3. **Give one short step at a time**, numbered ("1/3: look at your left wrist"). `notify` shows it as the game's
   HUD message with a sound for 8 seconds; repeat it every 30 seconds while the person has not reacted
   (`remind`, or a loop like `step()` in the example). Watch the game for the step's result where you can (a menu
   opened, a value changed) instead of asking.
4. **Ask in between** with buttons: `headset.ask(session, "Casing ok?", ["Yes", "No", "Not sure"])` shows the
   game's message box; the person clicks with the laser, the call returns the button's text. One question at a
   time.
5. **Use the signal for "done"** where the tool cannot see the result ("click STATS, then DATA. Hands up when
   done"). If a signal comes when no "done" was agreed, ask what it was for (a message box with your guesses as
   buttons) instead of guessing.
6. **Bundle topics.** Several checks, questions and feedback on the session itself in one run; number the steps
   so the person knows how far it is.
7. **Thank and switch off.** A last notification ("done, thank you!"), `signal off`, and the person quits the game.
8. **Keep the log.** Write each notification, question, answer and signal with a time stamp: it is the session's
   record, and the person can add what they noticed afterwards.

## Rules that proved useful

- **Short texts.** A HUD message is on screen only for its seconds; a long one is gone before it is read.
  Aim for a few words: "2/3: click STATS, then DATA".
- **Repeat, do not escalate.** No reaction after 30 seconds usually means the message was missed.
- **Questions cover the view.** A message box is a menu: depending on the game's settings and mods the world may
  pause, and it lies over an open Pip-Boy. Ask between steps, not while the person is in the middle of one.
- **Let the game tell you what happened** (`eval`, `state`, FNVVR's `pipboy`, the hovered interface element),
  and keep questions for what only the person can judge: looks, readability, feel.

## Example

[`examples/headset_session.py`](../examples/headset_session.py) is a complete session with three topics, shaped
after a real one. Its log:

```
15:40:27 notify: Hi! Raise both hands to signal me
15:40:33 notify: 1/4: look at your left wrist
15:40:44 step 1: done
15:40:48 ask: Casing and round corners ok? ['Yes', 'No', 'Not sure']
15:40:59 answer: Yes
15:40:59 ask: Close sign (top right) readable without the laser? ['Yes', 'No']
15:41:09 answer: Yes
15:41:09 notify: 3/4: click STATS, then DATA. Hands up when done
15:41:30 SIGNAL 1
15:41:30 ask: Did DATA open with one click? ['Yes', 'No']
15:41:34 answer: Yes
15:41:34 ask: Were my messages easy to read? ['Yes', 'Too short', 'Too long']
15:41:38 answer: Yes
15:41:38 notify: done
```

The core of it:

```python
import time
from fnvtest import Session, headset

with Session(game=True, vr=True) as s:
    headset.start(s, "Hi! Raise both hands to signal me")
    headset.remind(s, "1/3: look at your left wrist", done=lambda: headset.pipboy(s)["state"] == "open")
    headset.ask(s, "Does the Pip-Boy panel look right?", ["Yes", "No", "Not sure"])
    headset.notify(s, "2/3: click STATS, then DATA. Hands up when done")
    while not headset.signal_count(s):
        time.sleep(0.3)
    headset.ask(s, "Did DATA open with one click?", ["Yes", "No"])
    headset.notify(s, "3/3: done, thank you!")
    s.vr.ask("signal off")
```

## What the person sees

The greeting in the HUD (`headset.start`):

<img src="https://github.com/user-attachments/assets/2a834f85-3c2d-496a-8938-92017321d6fb" width="400" alt="The greeting as the game's HUD message: Raise both hands to signal me">

A question in the game's message box (`ask`), answered with the laser:

<img src="https://github.com/user-attachments/assets/440124fa-b395-4867-80d1-dfe853a1c849" width="360" alt="A question with Yes and No buttons in the game's message box">

The Pip-Boy on its panel in FNVVR, which a session opened and checked:

<img src="https://github.com/user-attachments/assets/d8217b62-b697-4665-bc0c-8ae581a5c0d7" width="400" alt="The Pip-Boy shown on FNVVR's menu panel with its casing and lamp bar">

(Emulator captures, cropped so that little of the game shows; the images live in
[issue #1](https://github.com/joschi127/FNVVR-Test-Bridge/issues/1), not in the repository.)
