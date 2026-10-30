import me.arnee.waddle.Mesh;

// Loads the phone app's C++ (built by build.sh) and calls every native method
// once: the names match Mesh.kt, and a phone that is not paired behaves.
public class Smoke {
    static void check(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        String state = Mesh.start(args[1]);
        check(state.contains("\"configured\":false"), state);
        check(Mesh.conversations().equals("[]"), "no conversations before pairing");
        check(Mesh.chat(0).equals("[]"), "no chat");
        check(!Mesh.canWrite(0) && !Mesh.send(0, "hei"), "cannot write before pairing");
        Mesh.markRead(0);
        Mesh.changes();
        Mesh.title(0);
        check(Mesh.members().equals("[]"), "no members before pairing");
        check(Mesh.createGroup(2, "Tur") == -1, "no group before pairing");
        check(Mesh.group(0).equals("null"), "Alle is not a group");
        Mesh.leaveGroup(33);
        check(Mesh.server().contains("\"url\":\"\""), "no server");
        check(Mesh.syncRequest().length > 0, "a sync request");
        check(Mesh.syncAnswer(new byte[0], 0) == 0, "an empty answer");
        check(!Mesh.startPoll(0, "Middag?", "Kl 18\nKl 19") && !Mesh.vote(0, 2, 7, 0), "no poll before pairing");
        check(!Mesh.startGame(0) && !Mesh.move(0, 2, 7, 3), "no game before pairing");
        String commit = Mesh.pairBegin();
        check(commit.length() == 64, commit);
        check(Mesh.pairAnswer("zz", "zz").equals("{}"), "a bad answer is refused");
        check(!Mesh.pairFinish("00").isEmpty(), "a bad box is refused");
        System.out.println("phone JNI: all methods load and answer");
    }
}
