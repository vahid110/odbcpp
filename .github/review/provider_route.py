"""Choose a reviewer without replacing a verdict, even after a failed action."""
import os


def route(outcome, result):
    if outcome not in {"success", "failure"}:
        raise ValueError("Claude review did not run")
    if result.strip():
        # Publication validates all nonempty responses, including malformed ones.
        return "claude"
    if outcome == "failure":
        return "openai"
    raise ValueError("Successful Claude action produced no review")


if __name__ == "__main__":
    print(route(os.environ["CLAUDE_OUTCOME"], os.environ["RESULT"]))
