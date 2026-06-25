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
let dictOrder = []; // store dict names in order

// Dict order is now persisted by C++ backend (dict_order.json file)

// ============================================================================
// Dictionary info & bar
// ============================================================================

// Load dictionary info
async function loadDictInfo() {
    try {
        const info = await __tauricpp__.invoke('dict_info', {});
        if (info.error) {
            dictInfo.textContent = 'Error: ' + info.error;
            statusText.textContent = 'Error';
            return;
        }
        if (info.loaded) {
            dictInfo.textContent = `${info.dict_count} dictionaries, ${info.word_count.toLocaleString()} words`;
            statusText.textContent = `Ready - ${info.dict_count} dictionaries (${info.word_count.toLocaleString()} words)`;
            if (!dictBarLoaded) loadDictBar();
        } else if (info.progress_total > 0) {
            const cached = info.progress_cached ? '' : ' (indexing...)';
            dictInfo.textContent = `Loading ${info.progress_current}/${info.progress_total}: ${info.progress_dict}${cached}`;
            statusText.textContent = `Loading dictionaries ${info.progress_current}/${info.progress_total}${cached}`;
            setTimeout(loadDictInfo, 300);
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
        if (!Array.isArray(list)) return;
        const bar = document.getElementById('dictBar');
        if (!bar) return;
        bar.innerHTML = '';

        // dict_list already returns items in saved order (C++ handles persistence)
        dictOrder = list.map(d => d.name);

        list.forEach(d => {
            const item = createDictBarItem(d);
            bar.appendChild(item);
        });

        dictBarLoaded = true;
    } catch (e) {}
}

function createDictBarItem(d) {
    const item = document.createElement('div');
    item.className = 'dict-bar-item';
    item.draggable = true;
    item.dataset.name = d.name;
    item.title = `${d.name} (${d.word_count.toLocaleString()} 词)`;
    if (d.icon) {
        const img = document.createElement('img');
        img.src = d.icon;
        item.appendChild(img);
    }
    const span = document.createElement('span');
    span.textContent = d.name + ' (' + d.word_count.toLocaleString() + ')';
    item.appendChild(span);

    // Drag events
    item.addEventListener('dragstart', (e) => {
        e.dataTransfer.setData('text/plain', d.name);
        e.dataTransfer.effectAllowed = 'move';
        item.classList.add('dragging');
    });
    item.addEventListener('dragend', () => {
        item.classList.remove('dragging');
        document.querySelectorAll('.dict-bar-item.drag-over').forEach(el => el.classList.remove('drag-over'));
    });
    item.addEventListener('dragover', (e) => {
        e.preventDefault();
        e.dataTransfer.dropEffect = 'move';
        const dragging = document.querySelector('.dict-bar-item.dragging');
        if (dragging && dragging !== item) {
            item.classList.add('drag-over');
        }
    });
    item.addEventListener('dragleave', () => {
        item.classList.remove('drag-over');
    });
    item.addEventListener('drop', async (e) => {
        e.preventDefault();
        item.classList.remove('drag-over');
        const draggedName = e.dataTransfer.getData('text/plain');
        if (draggedName === d.name) return;

        const bar = document.getElementById('dictBar');
        const items = [...bar.children];
        const draggedIdx = items.findIndex(el => el.dataset.name === draggedName);
        const targetIdx = items.findIndex(el => el.dataset.name === d.name);
        if (draggedIdx < 0 || targetIdx < 0) return;

        const draggedEl = items[draggedIdx];
        if (draggedIdx < targetIdx) {
            bar.insertBefore(draggedEl, items[targetIdx].nextSibling);
        } else {
            bar.insertBefore(draggedEl, items[targetIdx]);
        }
        // Update dictOrder and persist to C++ backend
        dictOrder = [...bar.children].map(el => el.dataset.name);
        try {
            await __tauricpp__.invoke('set_dict_order', { order: dictOrder });
        } catch (e) {}
    });
    return item;
}

// ============================================================================
// GoldenDict-mdx JavaScript API (called from MDX article HTML)
// ============================================================================

// toggle_active: used by Oxford MDX for expand/collapse of Extra Examples, etc.
// Called as toggle_active(this) where this = <span class="box_title">
// The is-active class goes on the parent .unbox element
window.toggle_active = function(x) {
    var e;
    if (typeof x === 'string') {
        e = document.getElementById(x);
    } else if (x && x.nodeType) {
        // x is the clicked element (e.g. box_title span)
        // Find the parent .unbox element and toggle is-active on it
        e = x.closest('.unbox') || x.closest('.collapse') || x;
    }
    if (e) {
        e.classList.toggle('is-active');
    }
};

// Also expose as bare function for onclick="toggle_active('...')" in MDX content
function toggle_active(x) { window.toggle_active(x); }

// ============================================================================
// Audio playback from MDD resources
// ============================================================================

// Make playAudio available globally for onclick handlers in article HTML
window.playAudio = async function(dictTitle, resourcePath) {
    console.log('playAudio called:', dictTitle, resourcePath);
    try {
        const result = await __tauricpp__.invoke('resource', { dict: dictTitle, path: resourcePath });
        console.log('resource result:', result.error ? 'error: ' + result.error : 'data length=' + (result.data || '').length);
        if (result.error || !result.data) {
            console.warn('playAudio: resource not found', dictTitle, resourcePath, result.error);
            return;
        }

        // Determine MIME type from extension
        const ext = resourcePath.split('.').pop().toLowerCase();
        const mimeMap = {
            mp3: 'audio/mpeg', wav: 'audio/wav', ogg: 'audio/ogg', oga: 'audio/ogg',
            m4a: 'audio/mp4', aac: 'audio/aac', au: 'audio/basic', voc: 'audio/voc',
            flac: 'audio/flac', wma: 'audio/x-ms-wma', speex: 'audio/speex'
        };
        const mime = mimeMap[ext] || 'audio/mpeg';
        console.log('playAudio: mime=' + mime + ', ext=' + ext);

        const audio = new Audio('data:' + mime + ';base64,' + result.data);
        audio.play().catch(e => console.warn('playAudio: playback failed', e));
    } catch (e) {
        console.warn('playAudio: error', e);
    }
};

// Event delegation: intercept clicks on links with data-sound attribute
// AND handle inline onclick from MDX content (e.g. Oxford Extra Examples toggle)
document.addEventListener('click', function(e) {
    // Handle data-sound links
    const link = e.target.closest('a[data-sound]');
    if (link) {
        e.preventDefault();
        const dictTitle = link.getAttribute('data-dict');
        const soundPath = link.getAttribute('data-sound');
        if (dictTitle && soundPath) {
            window.playAudio(dictTitle, soundPath);
        }
        return;
    }

    // Do NOT interfere with native <details>/<summary> toggle
    if (e.target.closest('summary') || e.target.closest('details')) {
        return;
    }

    // Handle div.collapse toggle (Oxford pattern: .box_title / pnc.heading click toggles .is-active)
    const collapseHeader = e.target.closest('.box_title, pnc.heading');
    if (collapseHeader) {
        const collapse = collapseHeader.closest('.collapse, div.collapse');
        if (collapse) {
            e.preventDefault();
            collapse.classList.toggle('is-active');
            return;
        }
    }

    // Handle inline onclick from MDX article content (e.g. Oxford "Extra Examples" toggle)
    const el = e.target;
    const clickable = el.hasAttribute('onclick') ? el : el.closest('[onclick]');
    if (clickable) {
        e.preventDefault();
        e.stopPropagation();
        try {
            const handler = clickable.getAttribute('onclick');
            if (handler) {
                const fn = new Function('event', handler);
                fn.call(clickable, e);
            }
        } catch (err) {
            console.warn('onclick failed:', err);
        }
    }
});

// ============================================================================
// Search & display
// ============================================================================

// Reorder dict-sections to match dictOrder
function reorderResults() {
    const container = contentArea.querySelector('.dict-results');
    if (!container || dictOrder.length === 0) return;
    const sections = [...container.querySelectorAll('.dict-section')];
    if (sections.length <= 1) return;

    // Sort sections by their position in dictOrder
    sections.sort((a, b) => {
        const aName = a.dataset.dict || '';
        const bName = b.dataset.dict || '';
        const aIdx = dictOrder.indexOf(aName);
        const bIdx = dictOrder.indexOf(bName);
        // Items not in dictOrder go last
        if (aIdx < 0 && bIdx < 0) return 0;
        if (aIdx < 0) return 1;
        if (bIdx < 0) return -1;
        return aIdx - bIdx;
    });

    // Re-append in sorted order
    sections.forEach(s => container.appendChild(s));
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
            // Reorder dict-sections to match dictOrder
            reorderResults();
            // Add collapse buttons to dict-name headers
            contentArea.querySelectorAll('.dict-name').forEach(nameEl => {
                const btn = document.createElement('span');
                btn.className = 'collapse-btn';
                btn.textContent = '\u25BC'; // ▼
                nameEl.insertBefore(btn, nameEl.firstChild);
                nameEl.addEventListener('click', () => {
                    const article = nameEl.nextElementSibling;
                    if (article && article.classList.contains('dict-article')) {
                        nameEl.classList.toggle('collapsed');
                        article.classList.toggle('collapsed');
                    }
                });
            });
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
