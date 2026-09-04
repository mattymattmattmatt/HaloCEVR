using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Text;
using System.Windows.Forms;

// HaloCEVR Config Editor - self-contained launcher.
// The editor HTML is compiled into this exe as an embedded resource.
// On run it writes the HTML to a stable per-user location and opens it
// in the default browser. No loose .html or .ico needs to ship with it.
static class Launcher
{
    [STAThread]
    static void Main()
    {
        try
        {
            Assembly asm = Assembly.GetExecutingAssembly();

            // Find the embedded .html resource (name ends with .html).
            string resName = null;
            foreach (string n in asm.GetManifestResourceNames())
            {
                if (n.EndsWith(".html", StringComparison.OrdinalIgnoreCase)) { resName = n; break; }
            }
            if (resName == null)
                throw new Exception("Embedded editor HTML was not found inside the executable.");

            string html;
            using (Stream s = asm.GetManifestResourceStream(resName))
            using (StreamReader r = new StreamReader(s, Encoding.UTF8))
                html = r.ReadToEnd();

            // Stable path so browser localStorage (revision history) persists across launches.
            string dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "HaloCEVR Config Editor");
            Directory.CreateDirectory(dir);
            string outPath = Path.Combine(dir, "halocevr-config-editor.html");

            // Rewrite each launch so exe updates ship the newest HTML.
            File.WriteAllText(outPath, html, new UTF8Encoding(false));

            // Open in whatever the user's default browser is.
            Process.Start(new ProcessStartInfo(outPath) { UseShellExecute = true });
        }
        catch (Exception ex)
        {
            MessageBox.Show(ex.Message, "HaloCEVR Config Editor",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }
}
