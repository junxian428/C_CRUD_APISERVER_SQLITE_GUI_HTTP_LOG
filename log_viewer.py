import tkinter as tk
from tkinter import scrolledtext
import time
import threading

class LogViewer(tk.Tk):
    def __init__(self, log_file):
        super().__init__()
        self.title("Log Viewer")
        self.geometry("600x400")
        
        self.log_file = log_file
        
        self.log_display = scrolledtext.ScrolledText(self, wrap=tk.WORD)
        self.log_display.pack(fill=tk.BOTH, expand=True)
        
        self.update_logs()

    def update_logs(self):
        with open(self.log_file, 'r') as file:
            content = file.read()
            self.log_display.delete('1.0', tk.END)
            self.log_display.insert(tk.END, content)
        self.after(1000, self.update_logs)  # Update logs every second

if __name__ == "__main__":
    log_file = "server.log"
    app = LogViewer(log_file)
    app.mainloop()
