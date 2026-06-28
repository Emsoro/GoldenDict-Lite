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

// ============================================================================
// IndexedDB persistence for dict order
// ============================================================================

const DB_NAME = 'goldendict-lite';
const DB_VERSION = 1;
const STORE_NAME = 'settings';

function openDB() {
    return new Promise((resolve, reject) => {
        const req = indexedDB.open(DB_NAME, DB_VERSION);
        req.onupgradeneeded = () => {
            req.result.createObjectStore(STORE_NAME);
        };
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
    });
}

async function loadDictOrderFromDB() {
    try {
        const db = await openDB();
        return new Promise((resolve) => {
            const tx = db.transaction(STORE_NAME, 'readonly');
            const req = tx.objectStore(STORE_NAME).get('dictOrder');
            req.onsuccess = () => resolve(Array.isArray(req.result) ? req.result : []);
            req.onerror = () => resolve([]);
        });
    } catch { return []; }
}

async function saveDictOrderToDB(order) {
    try {
        const db = await openDB();
        return new Promise((resolve) => {
            const tx = db.transaction(STORE_NAME, 'readwrite');
            tx.objectStore(STORE_NAME).put(order, 'dictOrder');
            tx.oncomplete = () => resolve();
            tx.onerror = () => resolve();
        });
    } catch {}
}

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

        // Load saved order from IndexedDB and apply to C++ backend
        const savedOrder = await loadDictOrderFromDB();
        if (savedOrder.length > 0) {
            await __tauricpp__.invoke('set_dict_order', { order: savedOrder });
            // Re-fetch list in saved order
            const orderedList = await __tauricpp__.invoke('dict_list', {});
            if (Array.isArray(orderedList)) {
                dictOrder = orderedList.map(d => d.name);
                orderedList.forEach(d => bar.appendChild(createDictBarItem(d)));
                dictBarLoaded = true;
                return;
            }
        }

        dictOrder = list.map(d => d.name);
        list.forEach(d => bar.appendChild(createDictBarItem(d)));
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
        // Update dictOrder and persist to IndexedDB
        dictOrder = [...bar.children].map(el => el.dataset.name);
        saveDictOrderToDB(dictOrder);
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
// Oxford image toggle: thumb <-> fullsize, pic_thumb <-> big_pic
// ============================================================================

// ox-enlarge: toggle between .thumb and .fullsize inside #ox-enlarge
window.toggle_enlarger = function(el) {
    var container = el.closest ? el : el;
    var thumb = container.querySelector('.thumb');
    var fullsize = container.querySelector('.fullsize');
    if (thumb && fullsize) {
        var thumbHidden = getComputedStyle(thumb).display === 'none';
        if (thumbHidden) {
            thumb.style.display = 'inline';
            fullsize.style.display = 'none';
        } else {
            thumb.style.display = 'none';
            fullsize.style.display = 'inline';
        }
    }
};
function toggle_enlarger(el) { window.toggle_enlarger(el); }

// pic_thumb click -> hide thumb, show big_pic
window.expand_big = function(el) {
    var pic = el.closest('.pic');
    if (pic) {
        el.style.display = 'none';
        var big = pic.querySelector('.big_pic');
        if (big) big.style.display = 'block';
    }
};

// big_pic click -> hide big_pic, show pic_thumb
window.expand_thumb = function(el) {
    var pic = el.closest('.pic');
    if (pic) {
        el.style.display = 'none';
        var thumb = pic.querySelector('.pic_thumb');
        if (thumb) thumb.style.display = 'block';
    }
};
function expand_big(el) { window.expand_big(el); }
function expand_thumb(el) { window.expand_thumb(el); }

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
    // Handle git-link clicks: open in default browser via TauriCPP
    const gitLink = e.target.closest('.git-link');
    if (gitLink) {
        e.preventDefault();
        const url = gitLink.getAttribute('href');
        if (url) {
            __tauricpp__.invoke('shell_open', { url });
        }
        return;
    }

    // Handle entry:// links (see also, topics, cross-references)
    const entryLink = e.target.closest('a[href^="entry://"]');
    if (entryLink) {
        e.preventDefault();
        const href = entryLink.getAttribute('href');
        const word = href.replace('entry://', '');
        if (word) {
            searchInput.value = word;
            doSearch(word);
        }
        return;
    }

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
            // Add collapse buttons and export buttons to dict-name headers
            contentArea.querySelectorAll('.dict-name').forEach(nameEl => {
                const btn = document.createElement('span');
                btn.className = 'collapse-btn';
                btn.textContent = '\u25BC'; // ▼
                nameEl.insertBefore(btn, nameEl.firstChild);

                // Add export button
                const exportBtn = document.createElement('button');
                exportBtn.className = 'export-btn';
                exportBtn.innerHTML = `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>`;
                exportBtn.title = '导出为 PNG';
                exportBtn.addEventListener('click', (e) => {
                    e.stopPropagation();
                    const section = nameEl.closest('.dict-section');
                    if (section) {
                        exportSectionAsPng(section);
                    }
                });
                nameEl.appendChild(exportBtn);

                nameEl.addEventListener('click', (e) => {
                    // Don't toggle if clicking export button
                    if (e.target.classList.contains('export-btn')) return;

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

// ============================================================================
// Export section as PNG with watermark
// ============================================================================

async function exportSectionAsPng(section) {
    const dictName = section.dataset.dict || 'Dictionary';
    const article = section.querySelector('.dict-article');
    const iconImg = section.querySelector('.dict-name img');
    
    if (!article) return;
    
    try {
        console.log('Starting export...');
        console.log('html2canvas available:', typeof html2canvas);
        
        if (typeof html2canvas === 'undefined') {
            alert('html2canvas 未加载，请检查网络连接后刷新页面');
            return;
        }
        
        // Use html2canvas to capture the article content
        console.log('Calling html2canvas...');
        const canvas = await html2canvas(article, {
            backgroundColor: '#ffffff',
            scale: 2,
            useCORS: true,
            logging: false
        });
        console.log('html2canvas completed, canvas size:', canvas.width, 'x', canvas.height);
        
        // Add watermark at top
        const watermarkHeight = 28;
        const finalCanvas = document.createElement('canvas');
        finalCanvas.width = canvas.width;
        finalCanvas.height = canvas.height + watermarkHeight * 2; // scale 2
        const finalCtx = finalCanvas.getContext('2d');
        
        // Draw watermark background at top (lighter)
        finalCtx.fillStyle = 'rgba(250, 248, 242, 0.95)';
        finalCtx.fillRect(0, 0, canvas.width, watermarkHeight * 2);
        
        // Draw border at bottom of watermark
        finalCtx.strokeStyle = '#e8e0c8';
        finalCtx.lineWidth = 1;
        finalCtx.beginPath();
        finalCtx.moveTo(0, watermarkHeight * 2);
        finalCtx.lineTo(canvas.width, watermarkHeight * 2);
        finalCtx.stroke();
        
        // Calculate text metrics first for right alignment
        finalCtx.font = 'italic 18px "等线", "DengXian", "Microsoft YaHei", sans-serif';
        const dictNameWidth = finalCtx.measureText(dictName).width;
        finalCtx.font = 'italic 17px "等线", "DengXian", "Microsoft YaHei", sans-serif';
        const gdWidth = finalCtx.measureText('GoldenDict ').width;
        finalCtx.font = 'italic bold 17px "等线", "DengXian", "Microsoft YaHei", sans-serif';
        const liteWidth = finalCtx.measureText('Lite').width;
        const gdTotalWidth = gdWidth + liteWidth;
        
        // Calculate total width and right-align position
        const iconSize = 20;
        const xSize = 12; // × line size
        const gap = 8;
        const smallGap = 12; // Smaller gap between dictName and X
        const totalWidth = iconSize + gap + dictNameWidth + smallGap + xSize + gap + gdTotalWidth + 20;
        const startX = canvas.width - totalWidth;
        const centerY = watermarkHeight;
        
        // Draw icon (aligned with text baseline)
        let currentX = startX;
        if (iconImg && iconImg.src) {
            try {
                const icon = new Image();
                await new Promise((resolve, reject) => {
                    icon.onload = resolve;
                    icon.onerror = reject;
                    icon.src = iconImg.src;
                });
                // Align icon vertically with text - adjust for 2x scale
                finalCtx.drawImage(icon, currentX, centerY - iconSize/2 - 2, iconSize, iconSize);
                currentX += iconSize + gap;
            } catch (e) {
                console.warn('Failed to load icon for watermark');
            }
        }
        
        // Draw text - dictName with black-gold to gold gradient (italic)
        const dictGradient = finalCtx.createLinearGradient(currentX, 0, currentX + dictNameWidth, 0);
        dictGradient.addColorStop(0, '#333');
        dictGradient.addColorStop(0.5, '#8a7a52');
        dictGradient.addColorStop(1, '#b8a472');
        finalCtx.fillStyle = dictGradient;
        finalCtx.font = 'italic 18px "等线", "DengXian", "Microsoft YaHei", sans-serif';
        finalCtx.textBaseline = 'middle';
        finalCtx.fillText(dictName, currentX, centerY);
        currentX += dictNameWidth + smallGap; // Use smaller gap
        
        // Draw × as modern graphic (wider angle, obtuse)
        finalCtx.strokeStyle = '#b8a472';
        finalCtx.lineWidth = 1.5;
        finalCtx.lineCap = 'round';
        const xCenterX = currentX + xSize/2;
        const xCenterY = centerY;
        // Wider X - larger vertical span for obtuse angle
        finalCtx.beginPath();
        finalCtx.moveTo(xCenterX - xSize/2, xCenterY - xSize/1.5);
        finalCtx.lineTo(xCenterX + xSize/2, xCenterY + xSize/1.5);
        finalCtx.moveTo(xCenterX + xSize/2, xCenterY - xSize/1.5);
        finalCtx.lineTo(xCenterX - xSize/2, xCenterY + xSize/1.5);
        finalCtx.stroke();
        currentX += xSize + gap;
        
        // Draw GoldenDict (gradient, italic)
        const gradient = finalCtx.createLinearGradient(currentX, 0, currentX + gdTotalWidth, 0);
        gradient.addColorStop(0, '#b8a472');
        gradient.addColorStop(1, '#8a7a52');
        finalCtx.fillStyle = gradient;
        finalCtx.font = 'italic 17px "等线", "DengXian", "Microsoft YaHei", sans-serif';
        finalCtx.fillText('GoldenDict ', currentX, centerY);
        
        // Draw Lite (bold, gradient)
        finalCtx.font = 'italic bold 17px "等线", "DengXian", "Microsoft YaHei", sans-serif';
        finalCtx.fillText('Lite', currentX + gdWidth, centerY);
        
        // Draw original content below watermark
        finalCtx.drawImage(canvas, 0, watermarkHeight * 2);
        
        console.log('Converting to dataURL...');
        // Convert to base64 and save via C++
        const dataUrl = finalCanvas.toDataURL('image/png');
        console.log('dataURL length:', dataUrl.length);
        
        const filename = `${dictName}_${Date.now()}.png`;
        
        console.log('Calling save_png...');
        const result = await __tauricpp__.invoke('save_png', {
            data: dataUrl,
            filename: filename
        });
        console.log('save_png result:', result);
        
        if (result.success) {
            showToast('图片已保存至：' + result.path);
        } else {
            showToast('保存失败：' + (result.error || '未知错误'), true);
        }
        
    } catch (e) {
        console.error('Export failed:', e);
        let errorMsg = '未知错误';
        if (e === undefined) {
            errorMsg = '错误对象为 undefined';
        } else if (e === null) {
            errorMsg = '错误对象为 null';
        } else if (e instanceof Error) {
            errorMsg = e.message + '\n' + e.stack;
        } else if (typeof e === 'string') {
            errorMsg = e;
        } else if (typeof e === 'object') {
            errorMsg = JSON.stringify(e, null, 2);
        } else {
            errorMsg = String(e);
        }
        showCustomDialog('导出失败：' + errorMsg);
    }
}

// Toast notification function
function showToast(message, isError = false) {
    const toast = document.createElement('div');
    toast.style.cssText = `
        position: fixed;
        top: 20px;
        left: 50%;
        transform: translateX(-50%);
        background: ${isError ? '#3a3020' : '#1a1a1a'};
        color: ${isError ? '#e8a040' : '#b8a472'};
        border: 1px solid ${isError ? '#8a6a30' : '#b8a472'};
        padding: 12px 24px;
        border-radius: 4px;
        font-size: 14px;
        z-index: 10000;
        opacity: 0;
        transition: opacity 0.3s;
        max-width: 80%;
        word-break: break-all;
    `;
    toast.textContent = message;
    document.body.appendChild(toast);
    
    setTimeout(() => toast.style.opacity = '1', 10);
    setTimeout(() => {
        toast.style.opacity = '0';
        setTimeout(() => document.body.removeChild(toast), 300);
    }, 3000);
}

// Init
loadDictInfo();
