const searchInput = document.getElementById('searchInput');
const searchBtn = document.getElementById('searchBtn');
const contentArea = document.getElementById('contentArea');
const suggestDropdown = document.getElementById('suggestDropdown');
const statusText = document.getElementById('statusText');
const dictInfo = document.getElementById('dictInfo');

let searchTimer = null;
let currentFocus = -1;
let isSearching = false;
let dictBarLoaded = false;

// Load dictionary info
async function loadDictInfo() {
    try {
        const info = await __tauricpp__.invoke('dict_info', {});
        if (info.loaded) {
            dictInfo.textContent = `${info.dict_count} dictionaries, ${info.word_count.toLocaleString()} words`;
            statusText.textContent = `Ready - ${info.dict_count} dictionaries (${info.word_count.toLocaleString()} words)`;
            if (!dictBarLoaded) loadDictBar();
        } else {
            dictInfo.textContent = 'Loading dictionaries...';
            statusText.textContent = 'Loading dictionaries...';
            setTimeout(loadDictInfo, 500);
        }
    } catch (e) {
        dictInfo.textContent = 'Error loading dictionary';
        statusText.textContent = 'Error';
    }
}

async function loadDictBar() {
    try {
        const list = await __tauricpp__.invoke('dict_list', {});
        const bar = document.getElementById('dictBar');
        bar.innerHTML = '';
        list.forEach(d => {
            const item = document.createElement('div');
            item.className = 'dict-bar-item';
            if (d.icon) {
                const img = document.createElement('img');
                img.src = d.icon;
                item.appendChild(img);
            }
            const span = document.createElement('span');
            span.textContent = d.name + ' (' + d.word_count.toLocaleString() + ')';
            item.appendChild(span);
            bar.appendChild(item);
        });
        dictBarLoaded = true;
    } catch (e) {}
}

// Search function
async function doSearch(word) {
    const q = word || searchInput.value.trim();
    if (!q || isSearching) return;
    hideSuggest();
    isSearching = true;
    statusText.textContent = 'Searching...';
    try {
        const result = await __tauricpp__.invoke('query', { word: q });
        if (result.error) {
            contentArea.innerHTML = `<div class="welcome"><p>${result.error}</p></div>`;
        } else {
            contentArea.innerHTML = `<div class="dict-results">${result.html}</div>`;
        }
        statusText.textContent = `Found: ${q}`;
    } catch (e) {
        contentArea.innerHTML = `<div class="welcome"><p>Error: ${e.message}</p></div>`;
        statusText.textContent = 'Search failed';
    }
    isSearching = false;
}

// Suggest function
async function doSuggest(prefix) {
    if (!prefix) { hideSuggest(); return; }
    try {
        const result = await __tauricpp__.invoke('suggest', { prefix: prefix, max: 15 });
        const words = result.words || [];
        if (words.length === 0) { hideSuggest(); return; }
        suggestDropdown.innerHTML = '';
        words.forEach((w) => {
            const item = document.createElement('div');
            item.className = 'suggest-item';
            item.textContent = w;
            item.addEventListener('mousedown', (e) => {
                e.preventDefault();
                searchInput.value = w;
                hideSuggest();
                doSearch(w);
            });
            suggestDropdown.appendChild(item);
        });
        suggestDropdown.classList.add('active');
        currentFocus = -1;
    } catch (e) {
        hideSuggest();
    }
}

function hideSuggest() {
    suggestDropdown.classList.remove('active');
    currentFocus = -1;
}

// Event listeners
searchInput.addEventListener('input', () => {
    clearTimeout(searchTimer);
    searchTimer = setTimeout(() => doSuggest(searchInput.value.trim()), 150);
});

searchInput.addEventListener('keydown', (e) => {
    const items = suggestDropdown.querySelectorAll('.suggest-item');
    if (e.key === 'Enter') {
        e.preventDefault();
        if (currentFocus >= 0 && items[currentFocus]) {
            searchInput.value = items[currentFocus].textContent;
        }
        hideSuggest();
        doSearch();
    } else if (e.key === 'ArrowDown') {
        e.preventDefault();
        currentFocus = Math.min(currentFocus + 1, items.length - 1);
        items.forEach((item, i) => item.style.background = i === currentFocus ? '#d4e4c4' : '');
    } else if (e.key === 'ArrowUp') {
        e.preventDefault();
        currentFocus = Math.max(currentFocus - 1, 0);
        items.forEach((item, i) => item.style.background = i === currentFocus ? '#d4e4c4' : '');
    } else if (e.key === 'Escape') {
        hideSuggest();
    }
});

searchInput.addEventListener('blur', () => {
    setTimeout(hideSuggest, 100);
});

searchBtn.addEventListener('click', () => doSearch());

// Init
loadDictInfo();
