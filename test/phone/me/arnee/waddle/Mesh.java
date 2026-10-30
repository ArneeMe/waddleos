package me.arnee.waddle;

// The same native methods as phone/.../Mesh.kt (a Kotlin object's @JvmStatic
// externals are static methods of class Mesh), to load the app's C++ on a computer.
public class Mesh {
    public static native String start(String dir);
    public static native int changes();
    public static native String conversations();
    public static native String chat(int conv);
    public static native String title(int conv);
    public static native boolean canWrite(int conv);
    public static native boolean send(int conv, String text);
    public static native void markRead(int conv);
    public static native String members();
    public static native int createGroup(long members, String name);
    public static native String group(int conv);
    public static native void leaveGroup(int conv);
    public static native byte[] syncRequest();
    public static native int syncAnswer(byte[] answer, long date);
    public static native String server();
    public static native String pairBegin();
    public static native String pairAnswer(String publicKey, String nonce);
    public static native String pairRequest();
    public static native String pairFinish(String box);
    public static native boolean startPoll(int conv, String question, String options);
    public static native boolean vote(int conv, int sender, long counter, int option);
    public static native boolean startGame(int conv);
    public static native boolean move(int conv, int sender, long counter, int col);
}
