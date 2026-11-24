"""Review histories scheduled by py-fsrs, for test_core.cpp to compare the C++ port against.
Run with the desktop program's Python:  python make_fsrs_vectors.py > fsrs_vectors.txt"""
import random
from datetime import datetime, timedelta, timezone

from fsrs import Card, Rating, Scheduler

sch = Scheduler(learning_steps=(), relearning_steps=(), enable_fuzzing=False)
rnd = random.Random(7)
t0 = datetime(2026, 1, 1, 12, tzinfo=timezone.utc)
for n in range(1000):
    card, day, out = Card(), rnd.randint(0, 30), []
    for _ in range(rnd.randint(1, 12)):
        rating = rnd.choices([1, 2, 3, 4], [2, 1, 6, 1])[0]
        card, _ = sch.review_card(card, Rating(rating), t0 + timedelta(days=day))
        due = (card.due - t0).days
        out.append(f"{day} {rating} {card.stability:.6f} {card.difficulty:.6f} {due}")
        day = max(day, due + rnd.choice([0, 0, 0, 1, 3, -1, 10])) if rnd.random() > 0.15 else day     # sometimes again the same day
    print(len(out), *out, sep=";")
