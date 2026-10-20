(() => {
  const API = "";
  const state = { token: localStorage.getItem("khan_token"), username: localStorage.getItem("khan_username") };

  const loginScreen = document.getElementById("loginScreen");
  const appScreen   = document.getElementById("appScreen");
  const loginForm   = document.getElementById("loginForm");
  const loginError  = document.getElementById("loginError");
  const usernameInput = document.getElementById("usernameInput");
  const userLabel   = document.getElementById("userLabel");
  const tally       = document.getElementById("tally");
  const composerForm = document.getElementById("composerForm");
  const taskInput   = document.getElementById("taskInput");
  const ledgerList  = document.getElementById("ledgerList");
  const signoutBtn  = document.getElementById("signoutBtn");

  function authHeaders() {
    return { "Authorization": "Bearer " + state.token, "Content-Type": "application/json" };
  }

  function showApp() {
    loginScreen.classList.add("hidden");
    appScreen.classList.add("visible");
    userLabel.textContent = state.username;
    loadTasks();
  }

  function showLogin(message) {
    state.token = null;
    state.username = null;
    localStorage.removeItem("khan_token");
    localStorage.removeItem("khan_username");
    appScreen.classList.remove("visible");
    loginScreen.classList.remove("hidden");
    loginError.textContent = message || "";
    usernameInput.focus();
  }

  function pad(n) { return String(n).padStart(3, "0"); }

  function renderTasks(tasks) {
    ledgerList.innerHTML = "";

    const done = tasks.filter(t => t.done).length;
    tally.textContent = tasks.length === 0
      ? "the ledger is empty"
      : `${done} / ${tasks.length} settled`;

    if (tasks.length === 0) {
      const empty = document.createElement("li");
      empty.className = "empty";
      empty.innerHTML = `<div class="big">—</div>open a line above to start the ledger`;
      ledgerList.appendChild(empty);
      return;
    }

    tasks.forEach(t => {
      const row = document.createElement("li");
      row.className = "row" + (t.done ? " done" : "");
      row.innerHTML = `
        <div class="idx">${pad(t.id)}</div>
        <button class="task-check" aria-label="${t.done ? 'mark not done' : 'mark done'}"></button>
        <div class="body">
          <div class="title"></div>
        </div>
        <button class="del" aria-label="delete">delete</button>
      `;
      row.querySelector(".title").textContent = t.title;

      row.querySelector(".task-check").addEventListener("click", () => toggleTask(t.id));
      row.querySelector(".del").addEventListener("click", () => deleteTask(t.id));
      ledgerList.appendChild(row);
    });
  }

  async function loadTasks() {
    const res = await fetch(API + "/tasks", { headers: authHeaders() });
    if (res.status === 401) { showLogin("session ended — open the ledger again"); return; }
    const tasks = await res.json();
    renderTasks(tasks);
  }

  async function toggleTask(id) {
    const res = await fetch(API + `/tasks/${id}/toggle`, { method: "POST", headers: authHeaders() });
    if (res.status === 401) { showLogin("session ended — open the ledger again"); return; }
    loadTasks();
  }

  async function deleteTask(id) {
    const res = await fetch(API + `/tasks/${id}`, { method: "DELETE", headers: authHeaders() });
    if (res.status === 401) { showLogin("session ended — open the ledger again"); return; }
    loadTasks();
  }

  loginForm.addEventListener("submit", async (e) => {
    e.preventDefault();
    const username = usernameInput.value.trim();
    if (!username) return;
    loginError.textContent = "";
    try {
      const res = await fetch(API + "/login", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ username })
      });
      if (!res.ok) { loginError.textContent = "couldn't open that ledger — try again"; return; }
      const data = await res.json();
      state.token = data.token;
      state.username = username;
      localStorage.setItem("khan_token", state.token);
      localStorage.setItem("khan_username", username);
      showApp();
    } catch (err) {
      loginError.textContent = "server unreachable";
    }
  });

  composerForm.addEventListener("submit", async (e) => {
    e.preventDefault();
    const title = taskInput.value.trim();
    if (!title) return;
    const res = await fetch(API + "/tasks", { method: "POST", headers: authHeaders(), body: JSON.stringify({ title }) });
    if (res.status === 401) { showLogin("session ended — open the ledger again"); return; }
    taskInput.value = "";
    loadTasks();
  });

  signoutBtn.addEventListener("click", () => showLogin());

  if (state.token && state.username) {
    showApp();
  } else {
    showLogin();
  }
})();
