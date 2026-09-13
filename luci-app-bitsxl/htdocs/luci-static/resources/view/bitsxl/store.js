'use strict';
'require view';
'require fs';
'require ui';

const BIN = '/usr/bin/bitsxl';
const MAX_CART = 3;
const CART_KEY = 'bitsxl.store.cart.v1';
const CUSTOM_HISTORY_KEY = 'bitsxl.store.custom.history.v1';
const BOOKMARK_KEY = 'bitsxl.store.bookmarks.v1';
const PAYMENT_LOG_KEY = 'bitsxl.payment.logs.v1';
const SOFT_LINE = 'rgba(127,127,127,.16)';
const SOFT_BORDER = 'linear-gradient(transparent,transparent) padding-box,linear-gradient(135deg,rgba(127,127,127,.26),rgba(127,127,127,.08),rgba(127,127,127,.20)) border-box';
const BUY_STYLE = 'background:#003b95;border-color:#003b95;color:#fff';
const ACTION_ROW_STYLE = 'display:flex;gap:.45em;flex-wrap:wrap;margin-top:.65em';
const PAYMENT_MODES = [
	{ value: 'balance', label: _('Balance'), command: 'balance' },
	{ value: 'balance-decoy-standard', label: _('Balance + Decoy'), command: 'balance-decoy-standard' },
	{ value: 'balance-decoy-v2', label: _('Balance + Decoy V2'), command: 'balance-decoy-v2' },
	{ value: 'qris', label: _('QRIS'), command: 'qris' },
	{ value: 'dana', label: _('DANA'), command: 'dana', ewallet: true, wallet: true },
	{ value: 'shopeepay', label: _('ShopeePay'), command: 'shopeepay', ewallet: true },
	{ value: 'gopay', label: _('GoPay'), command: 'gopay', ewallet: true },
	{ value: 'point', label: _('Point'), command: 'point', single: true },
	{ value: 'voucher', label: _('Voucher'), command: 'voucher', single: true },
	{ value: 'gift', label: _('Gift/Send Bonus'), command: 'gift', destination: true, single: true }
];
let cart = loadCart();
let bookmarks = loadBookmarks();
let contentBox = null;
let shopData = null;
let storeMode = 'main';
let packageSearch = '';
let customView = 'overview';
let customFamilyCode = '';
let customResult = null;
let customError = '';
let customLoading = false;
let familyListResult = null;
let familyListError = '';
let familyListLoading = false;
let pointResult = null;
let pointError = '';
let pointLoading = false;
let pointFamilyCode = '';
let pointFamilyResult = null;
let pointFamilyError = '';
let pointFamilyLoading = false;
let cartDragIndex = -1;
let cartFloatBox = null;
let cartModalBox = null;
let cartPointerDrag = null;
let searchUpdate = null;

function callBitsxl(args) {
	return L.resolveDefault(fs.exec_direct(BIN, args, 'json'), { ok: false, error: _('Unable to execute bitsxl') });
}

function notifyResult(res, okText) {
	if (res && res.ok)
		ui.addNotification(null, E('p', {}, okText || _('Done.')), 'info');
	else
		ui.addNotification(null, E('p', {}, (res && (res.error || res.message)) || _('Command failed.')), 'warning');
}

function copyText(text, ev) {
	if (ev) {
		ev.preventDefault();
		ev.stopPropagation();
	}
	text = String(text || '').trim();
	if (!text)
		return;
	const done = () => ui.addNotification(null, E('p', {}, _('Copied.')), 'info');
	const fallback = () => {
		const area = document.createElement('textarea');
		area.value = text;
		area.style.position = 'fixed';
		area.style.opacity = '0';
		document.body.appendChild(area);
		area.select();
		document.execCommand('copy');
		document.body.removeChild(area);
		done();
	};
	if (typeof navigator !== 'undefined' && navigator.clipboard && navigator.clipboard.writeText)
		return navigator.clipboard.writeText(text).then(done, fallback);
	fallback();
}

function logText(value) {
	if (value == null)
		return '';
	if (value instanceof Error)
		value = { message: value.message, stack: value.stack };
	if (typeof value === 'string')
		return value;
	try {
		return JSON.stringify(value, null, 2);
	} catch (err) {
		return String(value);
	}
}

function logField(payload, keys) {
	const stack = [ payload, payload && payload.redeemables, payload && payload.redeemables && payload.redeemables.response, payload && payload.redeemables && payload.redeemables.data, payload && payload.response, payload && payload.response && payload.response.response, payload && payload.response && payload.response.data ];
	for (let stackIndex = 0; stackIndex < stack.length; stackIndex++) {
		const item = stack[stackIndex];
		if (!item || typeof item !== 'object')
			continue;
		for (let index = 0; index < keys.length; index++)
			if (item[keys[index]] != null && item[keys[index]] !== '')
				return item[keys[index]];
	}
	return '';
}

function backendErrorMessage(res, fallback) {
	return logField(res, [ 'error', 'message', 'description', 'code_detail', 'code' ]) || fallback;
}

function savePaymentLog(source, mode, items, payload, customAmount) {
	if (typeof localStorage === 'undefined')
		return;
	try {
		const logs = JSON.parse(localStorage.getItem(PAYMENT_LOG_KEY) || '[]');
		const status = logField(payload, [ 'status', 'payment_status', 'code' ]) || (payload && payload.ok ? 'SUCCESS' : 'FAILED');
		const message = logField(payload, [ 'message', 'error', 'description', 'title', 'code_detail' ]);
		const quotedTotal = (items || []).reduce((sum, item) => sum + Number(item.price || 0), 0);
		const totalAmount = logField(payload, [ 'total_amount' ]) || customAmount || '';
		const customPrice = logField(payload, [ 'custom_price' ]) || (customAmount && Number(customAmount) !== quotedTotal ? customAmount : '');
		logs.unshift({
			time: Date.now(),
			source: source,
			payment: mode,
			status: status,
			message: message,
			quoted_total: quotedTotal,
			total_amount: totalAmount,
			custom_price: customPrice,
			items: (items || []).map((item) => ({ name: item.name || '', code: item.code || '', price: item.price || 0 })),
			response: payload
		});
		localStorage.setItem(PAYMENT_LOG_KEY, JSON.stringify(logs.slice(0, 200)));
	} catch (err) {}
}

function paymentError(message, payload) {
	const err = new Error(message || _('Payment failed.'));
	err.payload = payload;
	return err;
}

function showPaymentError(err) {
	const payload = err && err.payload != null ? err.payload : err;
	ui.showModal(_('Payment Error'), [
		E('div', {}, [
			E('div', { 'class': 'alert-message warning' }, err && err.message || _('Payment failed.')),
			E('pre', { 'style': 'margin-top:.75em;max-height:22em;overflow:auto;white-space:pre-wrap;word-break:break-word;background:rgba(127,127,127,.08);border:1px solid ' + SOFT_LINE + ';border-radius:6px;padding:.75em' }, logText(payload)),
			E('div', { 'style': 'display:flex;justify-content:flex-end;margin-top:1em' }, [
				E('button', { 'class': 'btn cbi-button', 'click': () => ui.hideModal() }, _('Close'))
			])
		])
	]);
}

function fmtMoney(value) {
	if (value == null || value === '')
		return 'IDR -';
	const number = Number(value);
	if (isNaN(number))
		return 'IDR ' + String(value);
	return 'IDR ' + String(Math.round(number)).replace(/\B(?=(\d{3})+(?!\d))/g, '.');
}

function fmtBytes(value) {
	value = Number(value || 0);
	const units = [ 'B', 'KB', 'MB', 'GB', 'TB' ];
	let index = 0;
	while (value >= 1024 && index < units.length - 1) {
		value /= 1024;
		index++;
	}
	return '%s %s'.format(value >= 10 || index === 0 ? value.toFixed(0) : value.toFixed(2), units[index]);
}

function fmtQuota(value, type) {
	type = String(type || '').toUpperCase();
	if (type === 'VOICE')
		return '%.2f %s'.format(Number(value || 0) / 60, _('minutes'));
	if (type === 'TEXT')
		return '%d SMS'.format(Number(value || 0));
	return fmtBytes(value);
}

function cleanPackageText(value) {
	return String(value || '')
		.replace(/<br\s*\/?>/gi, '\n')
		.replace(/<\/p>/gi, '\n')
		.replace(/<[^>]*>/g, ' ')
		.replace(/&nbsp;/g, ' ')
		.replace(/&amp;/g, '&')
		.replace(/&lt;/g, '<')
		.replace(/&gt;/g, '>')
		.replace(/\s+\n/g, '\n')
		.replace(/\n\s+/g, '\n')
		.replace(/[ \t]{2,}/g, ' ')
		.trim();
}

function firstValue(object, keys, fallback) {
	for (let index = 0; index < keys.length; index++) {
		const value = object && object[keys[index]];
		if (value != null && value !== '')
			return value;
	}
	return fallback;
}

function loadCart() {
	try {
		const raw = typeof window !== 'undefined' && window.sessionStorage && window.sessionStorage.getItem(CART_KEY);
		const rows = raw ? JSON.parse(raw) : [];
		return Array.isArray(rows) ? rows.slice(0, MAX_CART) : [];
	} catch (err) {
		return [];
	}
}

function saveCart() {
	try {
		if (typeof window !== 'undefined' && window.sessionStorage)
			window.sessionStorage.setItem(CART_KEY, JSON.stringify(cart.slice(0, MAX_CART)));
	} catch (err) {}
}

function bookmarkCode(item) {
	return String(firstValue(item, [ 'action_param', 'package_option_code', 'option_code', 'code' ], '') || '');
}

function bookmarkRecord(item) {
	const code = bookmarkCode(item);
	return {
		action_param: code,
		package_option_code: code,
		action_type: String(firstValue(item, [ 'action_type' ], 'PDP') || 'PDP'),
		title: firstValue(item, [ 'title', 'name', 'package_name' ], code),
		name: firstValue(item, [ 'name', 'title', 'package_name' ], code),
		family_name: firstValue(item, [ 'family_name', 'package_family_name' ], _('Bookmark')),
		package_family_name: firstValue(item, [ 'package_family_name', 'family_name' ], _('Bookmark')),
		package_family_code: firstValue(item, [ 'package_family_code', 'family_code' ], _('Bookmark')),
		price: packagePrice(item),
		original_price: firstValue(item, [ 'original_price', 'price' ], 0),
		discounted_price: firstValue(item, [ 'discounted_price' ], ''),
		validity: firstValue(item, [ 'validity' ], '') || '-',
		point: firstValue(item, [ 'point', 'points', 'reward_point' ], ''),
		priority: isPriorityItem(item),
		ts: Number(item && item.ts || Date.now())
	};
}

function loadBookmarks() {
	try {
		const raw = typeof window !== 'undefined' && window.localStorage && window.localStorage.getItem(BOOKMARK_KEY);
		const rows = raw ? JSON.parse(raw) : [];
		if (!Array.isArray(rows))
			return [];
		return rows.map(bookmarkRecord).filter((row) => bookmarkCode(row));
	} catch (err) {
		return [];
	}
}

function saveBookmarks(rows) {
	try {
		bookmarks = (rows || []).filter((row) => bookmarkCode(row));
		if (typeof window !== 'undefined' && window.localStorage)
			window.localStorage.setItem(BOOKMARK_KEY, JSON.stringify(bookmarks));
	} catch (err) {}
}

function isBookmarked(code) {
	code = String(code || '');
	return !!code && bookmarks.some((item) => bookmarkCode(item) === code);
}

function toggleBookmark(item, ev) {
	stopEvent(ev);
	const record = bookmarkRecord(item);
	const code = bookmarkCode(record);
	if (!code)
		return notifyResult({ ok: false, error: _('Missing package code.') });
	if (isBookmarked(code)) {
		saveBookmarks(bookmarks.filter((row) => bookmarkCode(row) !== code));
		notifyResult({ ok: true }, _('Bookmark removed.'));
	} else {
		saveBookmarks([ record ].concat(bookmarks.filter((row) => bookmarkCode(row) !== code)));
		notifyResult({ ok: true }, _('Bookmarked.'));
	}
	renderShopContent();
}

function normalizeFamilyCode(value) {
	return String(value || '').trim();
}

function familyCodeInputOk(code) {
	return !!code && code.length <= 128 && !/[\s\x00-\x1f\x7f]/.test(code);
}

function loadCustomHistory() {
	try {
		const raw = typeof window !== 'undefined' && window.localStorage && window.localStorage.getItem(CUSTOM_HISTORY_KEY);
		const rows = raw ? JSON.parse(raw) : [];
		if (!Array.isArray(rows))
			return [];
		return rows.map((row) => {
			if (typeof row === 'string')
				return { code: normalizeFamilyCode(row), ts: 0 };
			return { code: normalizeFamilyCode(row && row.code), title: String(row && row.title || '').trim(), ts: Number(row && row.ts || 0) };
		}).filter((row) => familyCodeInputOk(row.code)).slice(0, 30);
	} catch (err) {
		return [];
	}
}

function saveCustomHistory(rows) {
	try {
		if (typeof window !== 'undefined' && window.localStorage)
			window.localStorage.setItem(CUSTOM_HISTORY_KEY, JSON.stringify((rows || []).slice(0, 30)));
	} catch (err) {}
}

function rememberCustomFamily(code, title) {
	code = normalizeFamilyCode(code);
	if (!familyCodeInputOk(code))
		return;
	const upper = code.toUpperCase();
	const rows = loadCustomHistory().filter((row) => row.code.toUpperCase() !== upper);
	rows.unshift({ code: code, title: String(title || code).trim(), ts: Date.now() });
	saveCustomHistory(rows);
}

function removeCustomFamily(code) {
	const upper = normalizeFamilyCode(code).toUpperCase();
	saveCustomHistory(loadCustomHistory().filter((row) => row.code.toUpperCase() !== upper));
	renderShopContent();
}

function clearCustomHistory() {
	saveCustomHistory([]);
	renderShopContent();
}

function hasAny(text, words) {
	for (let index = 0; index < words.length; index++)
		if (text.indexOf(words[index]) >= 0)
			return true;
	return false;
}

function itemText(item) {
	return String([ familyName(item), firstValue(item, [ 'title', 'name', 'package_name' ], '') ].join(' ')).toUpperCase();
}

function isPriorityText(text) {
	return hasAny(String(text || '').toUpperCase(), [ 'PRIO', 'PRIORITAS', 'HYFE', 'MONTHLY BOOSTER', 'DAILY BOOSTER', 'APPS QUOTA' ]);
}

function subscriptionType() {
	return String(firstValue(shopData, [ 'subscription_type', 'substype', 'subscriber_type' ], '') || '').toUpperCase();
}

function isPriorityStore() {
	return hasAny(subscriptionType(), [ 'PRIO', 'PRIORITAS', 'HYBRID', 'POSTPAID' ]);
}

function isPriorityItem(item) {
	return !!(item && item.priority) || isPriorityStore() || isPriorityText(itemText(item));
}

function discountPercent(item) {
	const original = Number(item && item.original_price || 0);
	const discounted = Number(item && item.discounted_price || 0);
	if (original > 0 && discounted > 0 && discounted < original)
		return Math.round((original - discounted) * 100 / original);
	return 0;
}

function packageOriginalPrice(item) {
	return Number(item && item.original_price || 0);
}

function visibleStoreItems(data) {
	return filterPackages(shopItems(data));
}

function familyVariantItems(payload, root, fallbackCode) {
	const family = payload.package_family || root.package_family || {};
	const familyNameValue = firstValue(family, [ 'name', 'family_name' ], fallbackCode || _('Custom'));
	const familyCodeValue = firstValue(family, [ 'package_family_code', 'family_code', 'code' ], fallbackCode || familyNameValue);
	const currency = family.rc_bonus_type === 'MYREWARDS' ? 'point' : 'money';
	const items = [];
	(payload.package_variants || []).forEach((variant) => {
		(variant.package_options || []).forEach((option) => {
			const code = firstValue(option, [ 'package_option_code', 'option_code', 'action_param' ], '');
			items.push({
				title: compactTitle([ variant.name, option.name ], code || _('Package')),
				name: option.name,
				family_name: familyNameValue,
				package_family_name: familyNameValue,
				package_family_code: familyCodeValue,
				package_variant_code: variant.package_variant_code || '',
				package_option_code: code,
				action_param: code,
				action_type: 'PDP',
				price: firstValue(option, [ 'price', 'original_price' ], 0),
				original_price: firstValue(option, [ 'original_price', 'price' ], 0),
				discounted_price: option.discounted_price,
				currency: currency,
				payment_for: family.payment_for || '',
				validity: firstValue(option, [ 'validity' ], ''),
				point: firstValue(option, [ 'point', 'points', 'reward_point' ], ''),
				order: firstValue(option, [ 'order' ], 0)
			});
		});
	});
	return items;
}

function shopItems(data) {
	const root = data && data.shop || data || {};
	const payload = root.data || root;
	if (Array.isArray(payload.results_price_only))
		return payload.results_price_only;
	if (Array.isArray(payload.results))
		return payload.results;
	if (Array.isArray(payload.packages))
		return payload.packages;
	if (Array.isArray(payload.package_variants))
		return familyVariantItems(payload, root, data && data.family_code);
	return [];
}

function customFamilyTitle(data, fallback) {
	const root = data && data.shop || data || {};
	const payload = root.data || root;
	const family = payload.package_family || root.package_family || {};
	return firstValue(family, [ 'name', 'family_name', 'title' ], fallback) || fallback;
}

function packageDetailData(res) {
	const detail = res && res.detail;
	if (!detail || typeof detail !== 'object')
		return null;
	const payload = detail.data || detail.response && detail.response.data || detail.response || detail;
	return payload && typeof payload === 'object' ? payload : null;
}

function packageDetailError(res, detail) {
	const raw = res && (res.detail || res.response || res);
	const status = raw && raw.status != null ? String(raw.status).toUpperCase() : '';
	if (status && status !== 'SUCCESS')
		return firstValue(raw, [ 'message', 'error', 'description', 'code_detail', 'code' ], _('Package detail failed.'));
	if (!detail || !detail.package_option)
		return _('Package detail response missing package_option.');
	return '';
}

function rawDetailError(message, raw) {
	return E('div', {}, [
		E('div', { 'class': 'alert-message warning' }, message),
		E('details', { 'open': true, 'style': 'margin-top:.75em' }, [
			E('summary', { 'style': 'cursor:pointer;font-weight:650' }, _('Raw response')),
			E('pre', { 'style': 'margin-top:.6em;max-height:24em;overflow:auto;white-space:pre-wrap;word-break:break-word;background:rgba(127,127,127,.08);border:1px solid ' + SOFT_LINE + ';border-radius:6px;padding:.75em' }, logText(raw))
		])
	]);
}

function compactTitle(parts, fallback) {
	const seen = {};
	const out = [];
	(parts || []).forEach((part) => {
		part = String(part || '').trim();
		if (part && !seen[part]) {
			seen[part] = true;
			out.push(part);
		}
	});
	return out.length ? out.join(' - ') : fallback;
}

function familyCode(item) {
	return String(firstValue(item, [ 'package_family_code', 'family_code', 'family_id', 'family_name' ], _('Other')) || _('Other'));
}

function familyName(item) {
	return String(firstValue(item, [ 'family_name', 'family_label', 'package_family_name' ], familyCode(item)) || _('Other'));
}

function searchWords() {
	return String(packageSearch || '').trim().toUpperCase().split(/\s+/).filter((word) => word);
}

function filterBySearch(rows, textFn) {
	const words = searchWords();
	if (!words.length)
		return rows;
	return (rows || []).filter((row) => {
		const text = String(textFn(row) || '').toUpperCase();
		for (let index = 0; index < words.length; index++)
			if (text.indexOf(words[index]) < 0)
				return false;
		return true;
	});
}

function packageSearchText(item) {
	return [ firstValue(item, [ 'title', 'name', 'package_name' ], ''), familyCode(item) ].join(' ');
}

function filterPackages(items) {
	return filterBySearch(items, packageSearchText);
}

function searchPanel() {
	const input = E('input', {
		'id': 'bitsxl-package-search',
		'class': 'cbi-input-text',
		'type': 'search',
		'value': packageSearch,
		'placeholder': _('search package'),
		'aria-label': _('search package'),
		'style': 'width:100%;max-width:28em',
		'input': () => {
			packageSearch = input.value;
			if (searchUpdate)
				searchUpdate();
		}
	});
	return E('div', { 'style': 'display:flex;justify-content:center;margin:.2em 0 1em' }, [
		E('div', { 'style': 'width:100%;max-width:28em' }, [
			E('label', { 'for': 'bitsxl-package-search', 'style': 'display:block;font-size:.9em;color:inherit;opacity:.62;margin-bottom:.35em;text-align:center' }, _('search package')),
			input
		])
	]);
}

function emptyPackageMessage(fallback) {
	return E('div', { 'class': 'alert-message warning' }, searchWords().length ? _('No packages match search.') : fallback);
}

function packageResultsNode(items, fallback) {
	return items.length ? E('div', {}, items.map(packageRow)) : emptyPackageMessage(fallback);
}

function packageCode(item) {
	return String(firstValue(item, [ 'action_param', 'package_option_code', 'option_code' ], '') || '');
}

function packageCodeMeta(family, option) {
	const rows = [
		{ label: _('Family'), value: family },
		{ label: _('Option'), value: option }
	].filter((row) => row.value && row.value !== _('Other'));
	if (!rows.length)
		return '';
	return E('div', { 'style': 'display:flex;gap:.35em;flex-wrap:wrap;margin-top:.45em' }, rows.map((row) => E('span', { 'style': 'display:inline-flex;align-items:center;gap:.3em;min-width:0;max-width:100%;border:1px solid ' + SOFT_LINE + ';border-radius:4px;padding:.16em .35em;color:inherit;opacity:.72;background:rgba(127,127,127,.05);font-size:.86em' }, [
		E('span', { 'style': 'white-space:nowrap;font-weight:650' }, row.label),
		E('code', { 'style': 'min-width:0;max-width:12em;overflow:hidden;text-overflow:ellipsis;white-space:nowrap' }, row.value),
		E('button', { 'class': 'btn cbi-button', 'style': 'padding:.05em .35em;font-size:.9em', 'click': (ev) => copyText(row.value, ev) }, _('Copy'))
	])));
}

function packagePrice(item) {
	const discounted = Number(item && item.discounted_price || 0);
	if (discounted > 0)
		return discounted;
	return Number(firstValue(item, [ 'price', 'original_price' ], 0) || 0);
}

function packagePriceText(item) {
	const value = packagePrice(item);
	if (item && item.currency === 'point')
		return '%d %s'.format(Math.round(value), _('Poin'));
	return fmtMoney(value);
}

function totalPriceText(items, total) {
	if ((items || []).length && items.every((item) => item && item.currency === 'point'))
		return '%d %s'.format(Math.round(Number(total || 0)), _('Poin'));
	return fmtMoney(total);
}

function packageBenefitAmount(benefit) {
	if (benefit && benefit.is_unlimited)
		return _('Unlimited');
	return fmtQuota(benefit && benefit.total, benefit && benefit.data_type);
}

function packageBenefitRows(benefits) {
	if (!Array.isArray(benefits) || !benefits.length)
		return E('div', { 'class': 'alert-message warning', 'style': 'margin-top:.75em' }, _('No quota detail.'));
	return E('div', { 'style': 'margin-top:.45em;border-top:1px solid ' + SOFT_LINE }, benefits.map((benefit) => E('div', { 'style': 'display:grid;grid-template-columns:minmax(0,1fr) auto;gap:.5em 1em;align-items:center;padding:.75em 0;border-bottom:1px solid ' + SOFT_LINE }, [
		E('div', { 'style': 'min-width:0' }, [
			E('div', { 'style': 'font-weight:650;line-height:1.25' }, benefit.name || _('Benefit')),
			E('div', { 'style': 'color:inherit;opacity:.55;font-size:.9em;margin-top:.2em' }, benefit.data_type || 'DATA')
		]),
		E('div', { 'style': 'font-weight:700;color:#0645c8;white-space:nowrap;text-align:right' }, packageBenefitAmount(benefit))
	])));
}

function replaceChildren(node, child) {
	while (node.firstChild)
		node.removeChild(node.firstChild);
	if (child)
		node.appendChild(child);
}

function packageCartItem(actionParam, detail) {
	const option = detail.package_option || {};
	const family = detail.package_family || {};
	const variant = detail.package_detail_variant || {};
	const name = compactTitle([ family.name, variant.name, option.name ], actionParam);
	const priorityText = [ family.name, family.payment_for, family.plan_type, variant.name, option.name ].join(' ');
	return {
		code: actionParam,
		action_param: actionParam,
		package_option_code: actionParam,
		action_type: 'PDP',
		name: name,
		title: name,
		family_name: family.name || '',
		package_family_name: family.name || '',
		package_family_code: family.package_family_code || '',
		payment_for: family.payment_for || '',
		currency: family.rc_bonus_type === 'MYREWARDS' ? 'point' : 'money',
		price: option.price,
		priority: isPriorityStore() || isPriorityText(priorityText),
		validity: option.validity || '-',
		point: option.point == null ? '' : String(option.point)
	};
}

function packageListCartItem(item) {
	return {
		code: packageCode(item),
		name: firstValue(item, [ 'title', 'name', 'package_name' ], packageCode(item)),
		price: packagePrice(item),
		payment_for: firstValue(item, [ 'payment_for' ], ''),
		currency: item && item.currency || 'money',
		priority: isPriorityItem(item),
		validity: firstValue(item, [ 'validity' ], '-') || '-'
	};
}

function addToCart(item) {
	if (!item || !item.code)
		return notifyResult({ ok: false, error: _('Missing package code.') });
	if (cart.filter((row) => row.code === item.code).length)
		return notifyResult({ ok: false, error: _('Package already in cart.') });
	if (cart.length >= MAX_CART)
		return notifyResult({ ok: false, error: _('Cart maximum is 3 packages.') });
	cart.push(item);
	saveCart();
	renderCart();
	notifyResult({ ok: true }, _('Added to cart.'));
}

function removeFromCart(code) {
	cart = cart.filter((item) => item.code !== code);
	saveCart();
	renderCart();
}

function moveCartItem(from, to) {
	if (from < 0 || to < 0 || from >= cart.length || to >= cart.length || from === to)
		return;
	const row = cart.splice(from, 1)[0];
	cart.splice(to, 0, row);
	saveCart();
	renderCart();
}

function clearCart() {
	cart = [];
	saveCart();
	renderCart();
}

function cartIndex(code) {
	return cart.findIndex((item) => item && item.code === code);
}

function resetCartPointerDrag(commit) {
	const state = cartPointerDrag;
	if (!state)
		return;
	window.clearTimeout(state.timer);
	if (state.row) {
		state.row.style.transform = '';
		state.row.style.position = '';
		state.row.style.zIndex = '';
		state.row.style.boxShadow = '';
	}
	if (state.handle) {
		state.handle.style.cursor = 'grab';
		try {
			if (state.handle.hasPointerCapture(state.pointerId))
				state.handle.releasePointerCapture(state.pointerId);
		} catch (err) {}
	}
	if (state.targetRow)
		state.targetRow.style.outline = '';
	cartPointerDrag = null;
	if (commit && state.active && state.from !== state.to)
		moveCartItem(state.from, state.to);
}

function cartPointerDown(ev, row, rowsBox, code) {
	if (!ev || ev.pointerType === 'mouse' || (ev.button != null && ev.button !== 0))
		return;
	resetCartPointerDrag(false);
	const from = cartIndex(code);
	if (from < 0)
		return;
	const state = {
		active: false,
		pointerId: ev.pointerId,
		startX: ev.clientX,
		startY: ev.clientY,
		from: from,
		to: from,
		row: row,
		handle: ev.currentTarget,
		rowsBox: rowsBox,
		targetRow: null,
		centers: [],
		timer: null
	};
	try { state.handle.setPointerCapture(state.pointerId); } catch (err) {}
	state.timer = window.setTimeout(() => {
		if (cartPointerDrag !== state)
			return;
		state.active = true;
		state.centers = Array.from(rowsBox.querySelectorAll('[data-bitsxl-cart-code]')).map((node) => {
			const rect = node.getBoundingClientRect();
			return { node: node, y: (rect.top + rect.bottom) / 2 };
		});
		row.style.position = 'relative';
		row.style.zIndex = '2';
		row.style.boxShadow = '0 .35em 1em rgba(0,0,0,.18)';
		state.handle.style.cursor = 'grabbing';
	}, 320);
	cartPointerDrag = state;
}

function cartPointerMove(ev) {
	const state = cartPointerDrag;
	if (!state || ev.pointerId !== state.pointerId)
		return;
	if (!state.active) {
		if (Math.abs(ev.clientX - state.startX) > 10 || Math.abs(ev.clientY - state.startY) > 10)
			resetCartPointerDrag(false);
		return;
	}
	ev.preventDefault();
	state.row.style.transform = 'translateY(' + (ev.clientY - state.startY) + 'px)';
	let target = state.from;
	let distance = Infinity;
	state.centers.forEach((entry, index) => {
		const current = Math.abs(ev.clientY - entry.y);
		if (current < distance) {
			distance = current;
			target = index;
		}
	});
	if (state.targetRow)
		state.targetRow.style.outline = '';
	state.to = target;
	state.targetRow = state.centers[target] && state.centers[target].node;
	if (state.targetRow && state.targetRow !== state.row)
		state.targetRow.style.outline = '2px dashed currentColor';
}

function cartPointerEnd(ev, commit) {
	const state = cartPointerDrag;
	if (!state || ev.pointerId !== state.pointerId)
		return;
	if (state.active)
		ev.preventDefault();
	resetCartPointerDrag(commit);
}

function cartModalRow(item, rowsBox) {
	const row = E('div', {
		'data-bitsxl-cart-code': item.code,
		'style': 'display:grid;grid-template-columns:1.5em minmax(0,1fr) auto;gap:.55em;align-items:center;padding:.65em 0;border-bottom:1px solid ' + SOFT_LINE + ';transition:box-shadow .15s ease',
		'dragover': (ev) => {
			ev.preventDefault();
			if (ev.dataTransfer)
				ev.dataTransfer.dropEffect = 'move';
		},
		'drop': (ev) => {
			ev.preventDefault();
			moveCartItem(cartDragIndex, cartIndex(item.code));
			cartDragIndex = -1;
		}
	});
	const dragHandle = E('span', {
		'role': 'button',
		'tabindex': '0',
		'title': _('Hold and drag to reorder'),
		'aria-label': _('Hold and drag to reorder'),
		'draggable': true,
		'style': 'display:flex;align-items:center;justify-content:center;align-self:stretch;min-height:2.5em;font-size:1.15em;opacity:.55;cursor:grab;touch-action:none;user-select:none',
		'dragstart': (ev) => {
			cartDragIndex = cartIndex(item.code);
			if (ev.dataTransfer) {
				ev.dataTransfer.effectAllowed = 'move';
				ev.dataTransfer.setData('text/plain', item.code);
			}
		},
		'dragend': () => { cartDragIndex = -1; },
		'pointerdown': (ev) => cartPointerDown(ev, row, rowsBox, item.code),
		'pointermove': cartPointerMove,
		'pointerup': (ev) => cartPointerEnd(ev, true),
		'pointercancel': (ev) => cartPointerEnd(ev, false),
		'contextmenu': (ev) => ev.preventDefault()
	}, '\u2630');
	const detail = E('div', { 'style': 'min-width:0' }, [
			E('div', { 'style': 'font-weight:650;white-space:nowrap;overflow:hidden;text-overflow:ellipsis' }, '%d. %s'.format(cartIndex(item.code) + 1, item.name || item.code)),
			E('div', { 'style': 'display:flex;gap:.45em;align-items:center;flex-wrap:wrap;color:inherit;opacity:.62;font-size:.9em;margin-top:.15em' }, [
				E('span', {}, item.validity || '-'),
				E('span', { 'aria-hidden': 'true' }, '\u2022'),
				E('span', { 'style': 'font-weight:700;color:#0645c8;white-space:nowrap' }, packagePriceText(item))
			])
	]);
	row.appendChild(dragHandle);
	row.appendChild(detail);
	const index = cartIndex(item.code);
	row.appendChild(E('div', { 'style': 'display:grid;grid-template-columns:auto auto;gap:.25em;align-items:center' }, [
		E('button', { 'class': 'btn cbi-button', 'style': 'padding:.15em .45em', 'title': _('Move up'), 'aria-label': _('Move up'), 'disabled': index === 0 || null, 'click': () => moveCartItem(index, index - 1) }, '\u2191'),
		E('button', { 'class': 'btn cbi-button', 'style': 'padding:.15em .45em', 'title': _('Move down'), 'aria-label': _('Move down'), 'disabled': index === cart.length - 1 || null, 'click': () => moveCartItem(index, index + 1) }, '\u2193'),
		E('button', { 'class': 'btn cbi-button cbi-button-remove', 'style': 'grid-column:1 / 3', 'click': () => removeFromCart(item.code) }, _('Remove'))
	]));
	return row;
}

function cartModalContent() {
	const rowsBox = E('div', { 'style': 'border-top:1px solid ' + SOFT_LINE });
	cart.forEach((item) => rowsBox.appendChild(cartModalRow(item, rowsBox)));
	const total = cart.reduce((sum, item) => sum + Number(item.price || 0), 0);
	return E('div', {}, [
		cart.length ? E('div', { 'style': 'display:flex;justify-content:space-between;gap:.75em;align-items:center;margin-bottom:.7em;color:inherit;font-size:.9em' }, [
			E('span', { 'style': 'opacity:.65' }, _('Hold the handle and drag a package to change the checkout order.')),
			E('strong', { 'style': 'white-space:nowrap' }, '%d / %d'.format(cart.length, MAX_CART))
		]) : E('div', { 'class': 'alert-message warning' }, _('Cart is empty.')),
		cart.length ? rowsBox : '',
		cart.length ? E('div', { 'style': 'display:flex;justify-content:space-between;gap:1em;margin-top:.85em;font-weight:700' }, [ _('Total'), totalPriceText(cart, total) ]) : '',
		E('div', { 'style': 'display:flex;gap:.6em;justify-content:space-between;align-items:center;margin-top:1em;flex-wrap:wrap' }, [
			E('button', { 'class': 'btn cbi-button cbi-button-remove', 'disabled': !cart.length || null, 'click': clearCart }, _('Clear')),
			E('div', { 'class': 'right', 'style': 'display:flex;gap:.6em;flex-wrap:wrap' }, [
				E('button', { 'class': 'btn cbi-button', 'click': closeCartModal }, _('Close')),
				E('button', { 'class': 'btn cbi-button cbi-button-save', 'disabled': !cart.length || null, 'click': checkoutCartFromModal }, _('Checkout'))
			])
		])
	]);
}

function closeCartModal() {
	resetCartPointerDrag(false);
	cartModalBox = null;
	ui.hideModal();
}

function checkoutCartFromModal() {
	resetCartPointerDrag(false);
	cartModalBox = null;
	return checkoutPulsa();
}

function showCartModal() {
	resetCartPointerDrag(false);
	cartModalBox = E('div');
	replaceChildren(cartModalBox, cartModalContent());
	ui.showModal(_('Cart'), [ cartModalBox ]);
}

function renderFloatingCart() {
	if (!cartFloatBox)
		return;
	if (!cart.length) {
		replaceChildren(cartFloatBox, '');
		return;
	}
	const label = _('Cart') + ' (%d/%d)'.format(cart.length, MAX_CART);
	replaceChildren(cartFloatBox, E('button', {
		'class': 'btn cbi-button cbi-button-save',
		'type': 'button',
		'title': label,
		'aria-label': label,
		'style': 'position:relative;display:inline-flex;align-items:center;justify-content:center;width:3.2rem;height:3.2rem;min-width:3.2rem;padding:0;border-radius:50%;box-shadow:0 .4rem 1.2rem rgba(0,0,0,.28);font-size:1.35rem;line-height:1',
		'click': showCartModal
	}, [
		E('span', { 'aria-hidden': 'true' }, '\ud83d\uded2'),
		E('span', { 'aria-hidden': 'true', 'style': 'position:absolute;top:-.35rem;right:-.3rem;display:flex;align-items:center;justify-content:center;min-width:1.35rem;height:1.35rem;padding:0 .25rem;border-radius:999px;background:#d6263b;color:#fff;border:2px solid #fff;font-size:.72rem;font-weight:800;line-height:1;box-sizing:border-box' }, String(cart.length))
	]));
}

function renderCart() {
	renderFloatingCart();
	if (cartModalBox) {
		if (cartModalBox.isConnected)
			replaceChildren(cartModalBox, cartModalContent());
		else
			cartModalBox = null;
	}
}

function normalizeMsisdn(value) {
	value = String(value || '').trim().replace(/[^0-9]/g, '');
	if (value.indexOf('08') === 0)
		return '62' + value.slice(1);
	if (value.indexOf('8') === 0)
		return '62' + value;
	return value;
}

function normalizeWalletNumber(value) {
	value = String(value || '').trim().replace(/[^0-9]/g, '');
	if (value.indexOf('628') === 0)
		return '0' + value.slice(2);
	if (value.indexOf('8') === 0)
		return '0' + value;
	return value;
}

function paymentMode(value) {
	for (let index = 0; index < PAYMENT_MODES.length; index++)
		if (PAYMENT_MODES[index].value === value)
			return PAYMENT_MODES[index];
	return PAYMENT_MODES[0];
}

function paymentArgs(mode, items, walletNumber, destinationMsisdn, customAmount) {
	const selected = paymentMode(mode);
	const args = [ 'json', 'payment', selected.command ].concat(items.map((item) => item.code));
	if (selected.wallet)
		args.push(walletNumber);
	if (selected.destination)
		args.push(destinationMsisdn);
	if (customAmount && !selected.single)
		args.push('amount=' + customAmount);
	args.push('confirm=1');
	return args;
}

function customAmountValue(value) {
	value = String(value || '').trim();
	if (!value)
		return '';
	value = value.replace(/rp/ig, '').replace(/[.,\s]/g, '');
	if (!/^[0-9]+$/.test(value))
		return null;
	const number = Number(value);
	if (!Number.isSafeInteger(number) || number <= 0)
		return null;
	return value;
}

function paymentField(res, key) {
	if (!res)
		return '';
	if (res[key])
		return res[key];
	if (res.response && res.response[key])
		return res.response[key];
	if (res.response && res.response.data && res.response.data[key])
		return res.response.data[key];
	if (res.pending_detail && res.pending_detail[key])
		return res.pending_detail[key];
	if (res.pending_detail && res.pending_detail.data && res.pending_detail.data[key])
		return res.pending_detail.data[key];
	return '';
}

function paymentFollowupPanel(res, okText) {
	const qrisUrl = paymentField(res, 'qris_url');
	const qrisCode = paymentField(res, 'qris_code');
	const deeplink = paymentField(res, 'deeplink');
	const isQris = res && res.payment === 'qris' || qrisUrl || qrisCode;
	const isEwallet = res && res.payment === 'ewallet' || deeplink;
	if (!isQris && !isEwallet)
		return null;
	const link = isQris ? qrisUrl : deeplink;
	const raw = isQris ? qrisCode : deeplink;
	return E('div', {}, [
		E('div', { 'class': 'alert-message' }, okText || (isQris ? _('QRIS payment created.') : _('E-wallet payment created.'))),
		link ? E('div', { 'style': 'margin-top:.85em' }, [
			E('a', { 'class': 'btn cbi-button cbi-button-save', 'href': link, 'target': '_blank', 'rel': 'noopener' }, isQris ? _('Open QRIS') : _('Open payment'))
		]) : '',
		res.transaction_code ? E('div', { 'style': 'margin-top:.75em;color:inherit;opacity:.7;word-break:break-word' }, [ _('Transaction'), ': ', res.transaction_code ]) : '',
		raw ? E('pre', { 'style': 'margin-top:.75em;max-height:16em;overflow:auto;white-space:pre-wrap;word-break:break-word;background:rgba(127,127,127,.08);border:1px solid ' + SOFT_LINE + ';border-radius:6px;padding:.75em' }, raw) : '',
		E('details', { 'style': 'margin-top:.75em' }, [
			E('summary', { 'style': 'cursor:pointer;font-weight:650' }, _('Server response')),
			E('pre', { 'style': 'margin-top:.6em;max-height:20em;overflow:auto;white-space:pre-wrap;word-break:break-word;background:rgba(127,127,127,.08);border:1px solid ' + SOFT_LINE + ';border-radius:6px;padding:.75em' }, logText(res))
		]),
		E('div', { 'style': 'display:flex;justify-content:flex-end;margin-top:1em' }, [
			E('button', { 'class': 'btn cbi-button', 'click': () => ui.hideModal() }, _('Close'))
		])
	]);
}

function checkoutPulsaItems(items, options) {
	items = (items || []).filter((item) => item && item.code);
	options = options || {};
	if (!items.length)
		return notifyResult({ ok: false, error: _('No package selected.') });
	const total = items.reduce((sum, item) => sum + Number(item.price || 0), 0);
	const modeSelect = E('select', { 'class': 'cbi-input-select' }, PAYMENT_MODES.map((mode) => E('option', { 'value': mode.value }, mode.label)));
	if (options.defaultMode)
		modeSelect.value = paymentMode(options.defaultMode).value;
	const wallet = E('input', { 'class': 'cbi-input-text', 'type': 'text', 'placeholder': '08xxxx / 628xxxx', 'style': 'max-width:14em' });
	const destination = E('input', { 'class': 'cbi-input-text', 'type': 'text', 'placeholder': '08xxxx / 628xxxx', 'style': 'max-width:14em' });
	const customAmount = E('input', { 'class': 'cbi-input-text', 'type': 'text', 'inputmode': 'numeric', 'placeholder': String(total || 0), 'style': 'max-width:14em' });
	const walletRow = E('div', { 'style': 'display:none;margin-top:.6em' }, [ E('label', { 'style': 'display:block;font-weight:650;margin-bottom:.25em' }, _('Wallet number')), wallet ]);
	const destinationRow = E('div', { 'style': 'display:none;margin-top:.6em' }, [ E('label', { 'style': 'display:block;font-weight:650;margin-bottom:.25em' }, _('Destination MSISDN')), destination ]);
	const customAmountRow = E('div', { 'style': 'display:none;margin-top:.6em' }, [
		E('label', { 'style': 'display:block;font-weight:650;margin-bottom:.25em' }, _('Custom price')),
		customAmount,
		E('div', { 'style': 'margin-top:.25em;color:inherit;opacity:.62' }, _('Blank uses quoted total.'))
	]);
	const modeHint = E('div', { 'style': 'display:none;margin-top:.55em;color:inherit;opacity:.66' });
	const refreshModeInputs = () => {
		const selected = paymentMode(modeSelect.value);
		walletRow.style.display = selected.wallet ? 'block' : 'none';
		destinationRow.style.display = selected.destination ? 'block' : 'none';
		customAmountRow.style.display = selected.single ? 'none' : 'block';
		if (selected.value === 'qris') {
			modeHint.style.display = 'block';
			replaceChildren(modeHint, E('span', {}, _('QRIS link appears after checkout.')));
		} else if (selected.ewallet) {
			modeHint.style.display = 'block';
			replaceChildren(modeHint, E('span', {}, _('E-wallet payment link appears after checkout.')));
		} else {
			modeHint.style.display = 'none';
			replaceChildren(modeHint, '');
		}
	};
	modeSelect.addEventListener('change', refreshModeInputs);
	const buy = E('button', {
		'class': 'btn cbi-button cbi-button-save',
		'click': () => {
			const selected = paymentMode(modeSelect.value);
			const walletNumber = normalizeWalletNumber(wallet.value);
			const destinationMsisdn = normalizeMsisdn(destination.value);
			wallet.value = walletNumber;
			destination.value = destinationMsisdn;
			if (selected.single && items.length !== 1)
				return notifyResult({ ok: false, error: _('This payment mode supports one package only.') });
			if (selected.wallet && !walletNumber)
				return notifyResult({ ok: false, error: _('Wallet number required.') });
			if (selected.wallet && !/^08[0-9]{8,11}$/.test(walletNumber))
				return notifyResult({ ok: false, error: _('Wallet number must start with 08 and contain 10-13 digits.') });
			if (selected.destination && !destinationMsisdn)
				return notifyResult({ ok: false, error: _('Destination MSISDN required.') });
			if (selected.destination && !/^628[0-9]{5,11}$/.test(destinationMsisdn))
				return notifyResult({ ok: false, error: _('Destination MSISDN must start with 628.') });
			const customTotal = selected.single ? '' : customAmountValue(customAmount.value);
			if (customTotal == null)
				return notifyResult({ ok: false, error: _('Custom price must be a positive number.') });
			buy.disabled = true;
			let logged = false;
			return callBitsxl(paymentArgs(selected.value, items, walletNumber, destinationMsisdn, customTotal)).then((res) => {
				savePaymentLog('store', selected.value, items, res, customTotal);
				logged = true;
				if (!res || !res.ok)
					throw paymentError((res && (res.error || res.message)) || _('Checkout failed.'), res);
				if (options.clearCart) {
					cart = [];
					saveCart();
					renderCart();
				}
				const followup = paymentFollowupPanel(res, options.okText);
				if (followup) {
					replaceChildren(body, followup);
					return;
				}
				ui.hideModal();
				notifyResult({ ok: true }, options.okText || _('Checkout complete.'));
			}).catch((err) => {
				if (!logged)
					savePaymentLog('store', selected.value, items, err && err.payload != null ? err.payload : { ok: false, error: err && err.message || String(err) }, customTotal);
				buy.disabled = false;
				showPaymentError(err);
			});
		}
	}, _('Checkout'));
	const body = E('div', {}, [
		E('div', { 'style': 'border-top:1px solid ' + SOFT_LINE }, items.map((item) => E('div', { 'style': 'display:grid;grid-template-columns:minmax(0,1fr) auto;gap:1em;padding:.6em 0;border-bottom:1px solid ' + SOFT_LINE }, [
			E('div', { 'style': 'font-weight:650' }, item.name || item.code),
			E('div', { 'style': 'font-weight:700;color:#0645c8;white-space:nowrap' }, packagePriceText(item))
		]))),
		E('div', { 'style': 'display:flex;justify-content:space-between;gap:1em;margin-top:.8em;font-weight:700' }, [ _('Total'), totalPriceText(items, total) ]),
		E('div', { 'style': 'margin-top:1em' }, [ E('label', { 'style': 'display:block;font-weight:650;margin-bottom:.25em' }, _('Payment')), modeSelect ]),
		modeHint,
		customAmountRow,
		walletRow,
		destinationRow,
		E('div', { 'style': 'display:flex;gap:.6em;justify-content:flex-end;margin-top:1em;flex-wrap:wrap' }, [
			E('button', { 'class': 'btn cbi-button', 'click': () => ui.hideModal() }, _('Cancel')),
			buy
		])
	]);
	refreshModeInputs();
	ui.showModal(options.title || _('Checkout'), [ body ]);
}

function checkoutPulsa() {
	const defaultMode = cart.length === 1 && cart[0].payment_for === 'REDEEM_VOUCHER' ? 'point' : '';
	return checkoutPulsaItems(cart, { clearCart: true, title: _('Checkout'), defaultMode: defaultMode });
}

function stopEvent(ev) {
	if (ev) {
		ev.preventDefault();
		ev.stopPropagation();
	}
}

function directBuyItem(item, ev) {
	stopEvent(ev);
	const defaultMode = item && item.payment_for === 'REDEEM_VOUCHER' ? 'point' : '';
	return checkoutPulsaItems([ item ], { title: _('Buy Package'), okText: _('Purchase complete.'), defaultMode: defaultMode });
}

function directCartItem(item, ev) {
	stopEvent(ev);
	return addToCart(item);
}

function bookmarkButton(item, disabled) {
	const code = bookmarkCode(item);
	const bookmarked = isBookmarked(code);
	return E('button', {
		'class': 'btn cbi-button' + (bookmarked ? ' cbi-button-remove' : ''),
		'disabled': disabled || !code || null,
		'style': bookmarked ? 'background:rgba(180,35,24,.08);border-color:#b42318;color:#b42318' : '',
		'click': (ev) => toggleBookmark(item, ev)
	}, bookmarked ? _('Remove') : _('Bookmark'));
}

function showPackageDetail(actionType, actionParam, sourceItem) {
	if (actionType !== 'PDP' || !actionParam)
		return notifyResult({ ok: false, error: _('Unsupported package action.') });
	const body = E('div', {}, _('Loading package detail...'));
	ui.showModal(_('Package Detail'), [ body ]);
	const familyCode = firstValue(sourceItem || {}, [ 'package_family_code', 'family_code' ], '');
	const variantCode = firstValue(sourceItem || {}, [ 'package_variant_code', 'variant_code' ], '');
	const args = [ 'json', 'package', 'detail', actionParam ];
	if (familyCode || variantCode)
		args.push(familyCode || '-', variantCode || '-', isPriorityItem(sourceItem || {}) ? '1' : '0');
	return callBitsxl(args).then((res) => {
		if (!res || !res.ok) {
			replaceChildren(body, rawDetailError((res && res.error) || _('Failed to load package detail.'), res && (res.response || res.detail || res)));
			return;
		}
		const detail = packageDetailData(res);
		const detailError = packageDetailError(res, detail);
		if (detailError) {
			replaceChildren(body, rawDetailError(detailError, res && (res.detail || res.response || res)));
			return;
		}
		const option = detail.package_option || {};
		const family = detail.package_family || {};
		const variant = detail.package_detail_variant || {};
		const addon = detail.package_addon || {};
		const title = compactTitle([ family.name, variant.name, option.name ], actionParam);
		const tnc = cleanPackageText(option.tnc || option.description || '');
		const item = packageCartItem(actionParam, detail);
		const optionCode = option.package_option_code || actionParam;
		const familyCodeValue = family.package_family_code || family.family_code || '';
		replaceChildren(body, E('div', {}, [
			E('div', { 'style': 'font-weight:700;font-size:1.18em;line-height:1.25;margin-bottom:.25em' }, title),
			packageCodeMeta(familyCodeValue, optionCode),
			E('div', { 'style': 'border-top:1px solid ' + SOFT_LINE + ';margin:.75em 0 .8em' }),
			E('div', { 'style': 'display:grid;grid-template-columns:auto minmax(0,1fr);gap:.45em 1em;margin-bottom:1em' }, [
				E('div', {}, _('Price')), E('div', { 'style': 'font-weight:700;color:#0645c8' }, packagePriceText(item)),
				E('div', {}, _('Validity')), E('div', {}, option.validity || '-'),
				E('div', {}, _('Point')), E('div', {}, option.point == null ? '-' : String(option.point)),
				E('div', {}, _('Payment For')), E('div', {}, family.payment_for || '-'),
				E('div', {}, _('Plan Type')), E('div', {}, family.plan_type || '-'),
				E('div', {}, _('Parent Code')), E('div', {}, addon.parent_code || '-')
			]),
			E('div', { 'style': 'font-weight:700;margin:.2em 0 .45em' }, _('Quota Detail')),
			packageBenefitRows(option.benefits),
			tnc ? E('details', { 'style': 'margin-top:1em' }, [
				E('summary', { 'style': 'cursor:pointer;font-weight:650' }, _('Terms')),
				E('div', { 'style': 'white-space:pre-wrap;margin-top:.55em;color:inherit;opacity:.72;line-height:1.45' }, tnc)
			]) : '',
			E('div', { 'style': 'display:flex;align-items:center;justify-content:space-between;gap:.75em;margin-top:1.1em;flex-wrap:wrap' }, [
				E('div', { 'style': 'display:flex;gap:.6em;flex-wrap:wrap' }, [
					E('button', { 'class': 'btn cbi-button cbi-button-save', 'style': BUY_STYLE, 'click': (ev) => directBuyItem(item, ev) }, _('Buy')),
					E('button', { 'class': 'btn cbi-button', 'click': (ev) => directCartItem(item, ev) }, _('Cart')),
					bookmarkButton(item)
				]),
				E('button', { 'class': 'btn cbi-button', 'style': 'margin-left:auto', 'click': () => ui.hideModal() }, _('Close'))
			])
		]));
	});
}

function ribbonText(item, fallback) {
	return String(firstValue(item, [ 'ribbon', 'store_segment', 'label' ], fallback || '') || '');
}

function packageRow(item) {
	const title = firstValue(item, [ 'title', 'name', 'package_name' ], _('Package'));
	const validity = firstValue(item, [ 'validity' ], '');
	const actionType = String(firstValue(item, [ 'action_type' ], 'PDP') || 'PDP');
	const actionParam = packageCode(item);
	const ribbon = ribbonText(item, '');
	const point = firstValue(item, [ 'point', 'points', 'reward_point' ], '');
	const actionDisabled = actionType !== 'PDP' || !actionParam;
	return E('div', {
		'role': 'button',
		'tabindex': '0',
		'class': 'cbi-section',
		'style': 'margin:.75em 0;padding:.9em 1em;border:1px solid transparent;border-radius:6px;background:' + SOFT_BORDER + ';cursor:pointer',
		'click': () => showPackageDetail(actionType, actionParam, item),
		'keydown': (ev) => { if (ev.key === 'Enter' || ev.key === ' ') showPackageDetail(actionType, actionParam, item); }
	}, [
		ribbon ? E('div', { 'style': 'display:inline-block;margin-bottom:.55em;border:1px solid ' + SOFT_LINE + ';border-radius:4px;padding:.2em .45em;color:inherit;opacity:.7;font-size:.88em;font-weight:650;background:rgba(127,127,127,.06)' }, ribbon) : '',
		E('div', { 'style': 'display:grid;grid-template-columns:minmax(0,1fr) auto;gap:.75em;align-items:start' }, [
			E('div', { 'style': 'min-width:0' }, [
				E('div', { 'style': 'font-size:1.08em;font-weight:650;line-height:1.25' }, title),
				E('div', { 'style': 'margin-top:.3em;color:inherit;opacity:.6' }, familyName(item)),
				point ? E('div', { 'style': 'margin-top:.45em;color:inherit;opacity:.62;font-size:.92em' }, '+%s XL Poin'.format(point)) : ''
			]),
			E('div', { 'style': 'text-align:right;white-space:nowrap' }, [
				E('div', { 'style': 'font-weight:650;color:inherit;opacity:.72' }, validity || '-'),
				E('div', { 'style': 'margin-top:.45em;color:#0645c8;font-weight:700' }, packagePriceText(item))
			])
		]),
		E('div', { 'style': 'margin-top:.7em;border-top:1px solid ' + SOFT_LINE + ';padding-top:.55em;display:flex;align-items:center;justify-content:space-between;gap:.75em;flex-wrap:wrap' }, [
			E('div', { 'style': 'display:flex;gap:.45em;flex-wrap:wrap' }, [
				E('button', { 'class': 'btn cbi-button cbi-button-save', 'style': BUY_STYLE, 'disabled': actionDisabled || null, 'click': (ev) => directBuyItem(packageListCartItem(item), ev) }, _('Buy')),
				E('button', { 'class': 'btn cbi-button', 'disabled': actionDisabled || null, 'click': (ev) => directCartItem(packageListCartItem(item), ev) }, _('Cart')),
				bookmarkButton(item, actionDisabled)
			]),
			E('div', { 'style': 'margin-left:auto;text-align:right;color:inherit;opacity:.62;font-size:.9em' }, _('Tap to view detail'))
		])
	]);
}

function firstArray() {
	for (let index = 0; index < arguments.length; index++)
		if (Array.isArray(arguments[index]))
			return arguments[index];
	return null;
}

function familyListCode(row) {
	return String(firstValue(row, [ 'id', 'code', 'family_code', 'package_family_code' ], '') || '').trim();
}

function familyListLabel(row, code) {
	return String(firstValue(row, [ 'label', 'name', 'title', 'family_name', 'package_family_name' ], code) || code).trim();
}

function familyListShape(rows) {
	if (!rows.length)
		return true;
	return rows.some((row) => row && (row.id != null || row.label != null)) ||
		rows.every((row) => row && familyListCode(row) && !packageCode(row));
}

function familyListArray(data) {
	if (Array.isArray(data))
		return familyListShape(data) ? data : null;
	const roots = [ data, data && data.family_list, data && data.shop, data && data.response ];
	for (let index = 0; index < roots.length; index++) {
		const root = roots[index];
		const payload = root && root.data || root;
		const rows = firstArray(
			payload && payload.results,
			payload && payload.family_list,
			payload && payload.families,
			payload && payload.items,
			root && root.results
		);
		if (rows && familyListShape(rows))
			return rows;
	}
	return null;
}

function familyListRows(data) {
	const rows = familyListArray(data) || [];
	return rows.map((row) => {
		const code = familyListCode(row);
		return {
			code: code,
			label: familyListLabel(row, code)
		};
	}).filter((row) => row.code);
}

function pointCategories(data) {
	const roots = [ data && data.redeemables, data && data.response, data && data.point, data ];
	for (let index = 0; index < roots.length; index++) {
		const root = roots[index];
		const payload = root && root.data || root;
		if (payload && Array.isArray(payload.categories))
			return payload.categories;
	}
	return null;
}

function pointActionType(row) {
	return String(firstValue(row, [ 'action_type' ], '') || '').toUpperCase();
}

function pointActionCode(row) {
	return String(firstValue(row, [ 'action_param', 'package_option_code', 'option_code', 'family_code', 'package_family_code' ], '') || '').trim();
}

function fmtDateSeconds(value) {
	const number = Number(value || 0);
	if (!number)
		return '-';
	const ms = number > 9999999999 ? number : number * 1000;
	try {
		return new Date(ms).toLocaleDateString();
	} catch (err) {
		return '-';
	}
}

function familyListOk(res) {
	return !!(res && res.ok && familyListArray(res));
}

function fetchFamilyList(force) {
	if (familyListLoading || (!force && familyListResult))
		return Promise.resolve(familyListResult);
	familyListLoading = true;
	familyListError = '';
	renderShopContent();
	return callBitsxl([ 'json', 'shop', 'family-list' ]).then((res) => {
		if (familyListOk(res))
			return res;
		return callBitsxl([ 'json', 'family-list' ]).then((fallback) => familyListOk(fallback) ? fallback : fallback || res);
	}).then((res) => {
		familyListLoading = false;
		if (!familyListOk(res)) {
			familyListError = (res && (res.error || res.message)) || _('Failed to load family list.');
			familyListResult = null;
			renderShopContent();
			return notifyResult({ ok: false, error: familyListError });
		}
		familyListResult = res;
		renderShopContent();
	}).catch((err) => {
		familyListLoading = false;
		familyListError = err.message || String(err);
		familyListResult = null;
		renderShopContent();
		notifyResult({ ok: false, error: familyListError });
	});
}

function pointOk(res) {
	return !!(res && res.ok && pointCategories(res));
}

function fetchPoint(force) {
	if (pointLoading || (!force && pointResult))
		return Promise.resolve(pointResult);
	pointLoading = true;
	pointError = '';
	renderShopContent();
	return callBitsxl([ 'json', 'point' ]).then((res) => {
		pointLoading = false;
		if (!pointOk(res)) {
			pointError = backendErrorMessage(res, _('Failed to load point catalog.'));
			pointResult = null;
			renderShopContent();
			return notifyResult({ ok: false, error: pointError });
		}
		pointResult = res;
		renderShopContent();
	}).catch((err) => {
		pointLoading = false;
		pointError = err.message || String(err);
		pointResult = null;
		renderShopContent();
		notifyResult({ ok: false, error: pointError });
	});
}

function fetchPointFamily(code) {
	code = normalizeFamilyCode(code);
	if (!familyCodeInputOk(code)) {
		pointFamilyError = _('Invalid family code.');
		renderShopContent();
		return notifyResult({ ok: false, error: pointFamilyError });
	}
	pointFamilyCode = code;
	pointFamilyLoading = true;
	pointFamilyError = '';
	pointFamilyResult = null;
	renderShopContent();
	return callBitsxl([ 'json', 'shop', 'family', code ]).then((res) => {
		pointFamilyLoading = false;
		if (!res || !res.ok) {
			pointFamilyError = backendErrorMessage(res, _('Failed to load point family.'));
			pointFamilyResult = null;
			renderShopContent();
			return notifyResult({ ok: false, error: pointFamilyError });
		}
		pointFamilyResult = res;
		renderShopContent();
	}).catch((err) => {
		pointFamilyLoading = false;
		pointFamilyError = err.message || String(err);
		pointFamilyResult = null;
		renderShopContent();
		notifyResult({ ok: false, error: pointFamilyError });
	});
}

function fetchCustomFamily(code) {
	code = normalizeFamilyCode(code);
	if (!familyCodeInputOk(code)) {
		customError = _('Invalid family code.');
		renderShopContent();
		return notifyResult({ ok: false, error: customError });
	}
	customFamilyCode = code;
	customLoading = true;
	customError = '';
	customResult = null;
	renderShopContent();
	return callBitsxl([ 'json', 'shop', 'family', code ]).then((res) => {
		customLoading = false;
		if (!res || !res.ok) {
			customError = (res && (res.error || res.message)) || _('Failed to load family code.');
			customResult = null;
			renderShopContent();
			return notifyResult({ ok: false, error: customError });
		}
		customResult = res;
		rememberCustomFamily(code, customFamilyTitle(res, code));
		renderShopContent();
	}).catch((err) => {
		customLoading = false;
		customError = err.message || String(err);
		customResult = null;
		renderShopContent();
		notifyResult({ ok: false, error: customError });
	});
}

function customInputPanel() {
	const input = E('input', {
		'class': 'cbi-input-text',
		'type': 'text',
		'value': customFamilyCode,
		'placeholder': _('Family code'),
		'style': 'width:100%;max-width:28em',
		'keydown': (ev) => { if (ev.key === 'Enter') fetchCustomFamily(input.value); }
	});
	return E('div', { 'style': 'margin:.3em 0 1em' }, [
		E('div', { 'style': 'font-size:.9em;color:inherit;opacity:.62;margin-bottom:.35em' }, _('input your family code here')),
		input,
		E('div', { 'style': 'display:flex;justify-content:center;margin-top:.65em' }, [
			E('button', { 'class': 'btn cbi-button cbi-button-save', 'style': BUY_STYLE, 'click': () => fetchCustomFamily(input.value) }, _('Fetch'))
		])
	]);
}

function customPackagesPanel() {
	if (customLoading)
		return E('div', { 'class': 'alert-message' }, _('Loading packages...'));
	if (customError)
		return E('div', { 'class': 'alert-message warning' }, customError);
	if (!customResult)
		return '';
	const results = E('div');
	const update = () => replaceChildren(results, packageResultsNode(filterPackages(shopItems(customResult)), _('No packages for this family code.')));
	searchUpdate = update;
	update();
	return E('div', {}, [
		searchPanel(),
		results,
		refreshButton()
	]);
}

function customOverviewPanel() {
	return E('div', {}, [
		customInputPanel(),
		customPackagesPanel()
	]);
}

function customDate(ts) {
	if (!ts)
		return '-';
	try {
		return new Date(ts).toLocaleString();
	} catch (err) {
		return '-';
	}
}

function customHistoryPanel() {
	const rows = loadCustomHistory();
	return E('div', {}, [
		rows.length ? E('div', { 'style': 'border-top:1px solid ' + SOFT_LINE }, rows.map((row) => E('div', { 'style': 'display:grid;grid-template-columns:minmax(0,1fr) auto auto;gap:.55em;align-items:center;padding:.65em 0;border-bottom:1px solid ' + SOFT_LINE }, [
			E('div', { 'style': 'min-width:0' }, [
				E('div', { 'style': 'font-weight:650;white-space:nowrap;overflow:hidden;text-overflow:ellipsis' }, row.title || row.code),
				E('div', { 'style': 'color:inherit;opacity:.55;font-size:.9em;white-space:nowrap;overflow:hidden;text-overflow:ellipsis' }, [ row.code, ' - ', customDate(row.ts) ])
			]),
			E('button', { 'class': 'btn cbi-button cbi-button-save', 'style': BUY_STYLE, 'click': () => fetchCustomFamily(row.code) }, _('Fetch')),
			E('button', { 'class': 'btn cbi-button cbi-button-remove', 'click': () => removeCustomFamily(row.code) }, _('Remove'))
		]))) : E('div', { 'class': 'alert-message warning' }, _('No family code history.')),
		rows.length ? E('button', { 'class': 'btn cbi-button cbi-button-remove', 'style': 'margin-top:1em', 'click': clearCustomHistory }, _('Clear')) : '',
		customPackagesPanel()
	]);
}

function familyListPanel() {
	if (!familyListResult && !familyListLoading && !familyListError)
		window.setTimeout(() => fetchFamilyList(), 0);
	const results = E('div');
	const renderRows = () => {
		const rows = filterBySearch(familyListRows(familyListResult), (row) => [ row.label, row.code ].join(' '));
		return rows.length ? E('div', { 'style': 'border-top:1px solid ' + SOFT_LINE }, rows.map((row) => E('div', { 'style': 'display:grid;grid-template-columns:minmax(0,1fr) auto;gap:.55em;align-items:center;padding:.65em 0;border-bottom:1px solid ' + SOFT_LINE }, [
			E('div', { 'style': 'min-width:0' }, [
				E('div', { 'style': 'font-weight:650;white-space:nowrap;overflow:hidden;text-overflow:ellipsis' }, row.label || row.code),
				E('div', { 'style': 'color:inherit;opacity:.55;font-size:.9em;white-space:nowrap;overflow:hidden;text-overflow:ellipsis' }, row.code)
			]),
			E('button', { 'class': 'btn cbi-button cbi-button-save', 'style': BUY_STYLE, 'click': () => { customView = 'overview'; fetchCustomFamily(row.code); } }, _('Fetch'))
		]))) : E('div', { 'class': 'alert-message warning' }, searchWords().length ? _('No family list entries match search.') : _('No family list.'));
	};
	const update = () => replaceChildren(results, renderRows());
	searchUpdate = update;
	update();
	return E('div', {}, [
		searchPanel(),
		familyListLoading ? E('div', { 'class': 'alert-message' }, _('Loading family list...')) : '',
		familyListError ? E('div', { 'class': 'alert-message warning' }, familyListError) : '',
		!familyListLoading && !familyListError ? results : '',
		refreshButton()
	]);
}

function pointPackageItem(row, categoryName) {
	const code = pointActionCode(row);
	const name = firstValue(row, [ 'name', 'title', 'package_name' ], code);
	return {
		action_param: code,
		package_option_code: code,
		action_type: pointActionType(row) || 'PDP',
		title: name,
		name: name,
		family_name: categoryName || _('Point'),
		package_family_name: categoryName || _('Point'),
		package_family_code: firstValue(row, [ 'package_family_code', 'family_code' ], ''),
		price: firstValue(row, [ 'price', 'point', 'points' ], 0),
		validity: fmtDateSeconds(firstValue(row, [ 'valid_until', 'expired_at' ], 0)),
		point: firstValue(row, [ 'point', 'points', 'reward_point' ], '')
	};
}

function pointRedeemableRow(row, categoryName) {
	const type = pointActionType(row);
	const code = pointActionCode(row);
	const title = firstValue(row, [ 'name', 'title', 'package_name' ], _('Reward'));
	const valid = fmtDateSeconds(firstValue(row, [ 'valid_until', 'expired_at' ], 0));
	const item = pointPackageItem(row, categoryName);
	const open = (ev) => {
		stopEvent(ev);
		if (type === 'PDP')
			return showPackageDetail('PDP', code, item);
		if (type === 'PLP')
			return fetchPointFamily(code);
		return notifyResult({ ok: false, error: _('Unsupported package action.') });
	};
	return E('div', {
		'role': 'button',
		'tabindex': '0',
		'style': 'display:grid;grid-template-columns:minmax(0,1fr) auto;gap:.7em;align-items:center;padding:.75em 0;border-bottom:1px solid ' + SOFT_LINE + ';cursor:pointer',
		'click': open,
		'keydown': (ev) => { if (ev.key === 'Enter' || ev.key === ' ') open(ev); }
	}, [
		E('div', { 'style': 'min-width:0' }, [
			E('div', { 'style': 'font-weight:650;line-height:1.25;overflow-wrap:anywhere' }, title),
			E('div', { 'style': 'color:inherit;opacity:.58;font-size:.9em;margin-top:.25em' }, [
				categoryName || _('Point'), ' - ', valid, type ? ' - ' + type : ''
			])
		]),
		E('button', { 'class': 'btn cbi-button cbi-button-save', 'style': BUY_STYLE, 'disabled': !code || null, 'click': open }, type === 'PLP' ? _('Packages') : _('Detail'))
	]);
}

function pointFamilyPanel() {
	const results = E('div');
	const update = () => replaceChildren(results, packageResultsNode(filterPackages(shopItems(pointFamilyResult)), _('No packages for this point family.')));
	searchUpdate = update;
	update();
	return E('div', {}, [
		E('div', { 'style': 'display:flex;justify-content:space-between;gap:.75em;align-items:center;flex-wrap:wrap;margin-bottom:1em' }, [
			E('div', { 'style': 'font-weight:700;overflow-wrap:anywhere' }, pointFamilyCode),
			E('button', { 'class': 'btn cbi-button', 'click': () => { pointFamilyCode = ''; pointFamilyResult = null; pointFamilyError = ''; renderShopContent(); } }, _('Back'))
		]),
		pointFamilyLoading ? E('div', { 'class': 'alert-message' }, _('Loading packages...')) : '',
		pointFamilyError ? E('div', { 'class': 'alert-message warning' }, pointFamilyError) : '',
		!pointFamilyLoading && !pointFamilyError ? E('div', {}, [ searchPanel(), results ]) : '',
		refreshButton()
	]);
}

function pointPanel() {
	if (pointFamilyCode || pointFamilyLoading || pointFamilyError)
		return pointFamilyPanel();
	if (!pointResult && !pointLoading && !pointError)
		window.setTimeout(() => fetchPoint(), 0);
	const results = E('div');
	const renderRows = () => {
		const categories = pointCategories(pointResult) || [];
		const words = searchWords();
		const nodes = [];
		categories.forEach((category) => {
			const categoryName = firstValue(category, [ 'category_name', 'name', 'title' ], _('Point'));
			const rows = filterBySearch(category.redeemables || [], (row) => [ categoryName, firstValue(row, [ 'name', 'title', 'package_name' ], ''), pointActionCode(row), pointActionType(row) ].join(' '));
			if (!rows.length && words.length)
				return;
			nodes.push(E('div', { 'class': 'cbi-section', 'style': 'margin:.75em 0;padding:.9em 1em;border:1px solid transparent;border-radius:6px;background:' + SOFT_BORDER }, [
				E('div', { 'style': 'display:flex;justify-content:space-between;gap:.75em;align-items:center;flex-wrap:wrap;margin-bottom:.35em' }, [
					E('div', { 'style': 'font-weight:700;font-size:1.05em' }, categoryName),
					category.category_code ? E('code', { 'style': 'color:inherit;opacity:.55' }, category.category_code) : ''
				]),
				rows.length ? E('div', { 'style': 'border-top:1px solid ' + SOFT_LINE }, rows.map((row) => pointRedeemableRow(row, categoryName))) : E('div', { 'class': 'alert-message warning' }, _('No redeemables in this category.'))
			]));
		});
		return nodes.length ? E('div', {}, nodes) : E('div', { 'class': 'alert-message warning' }, searchWords().length ? _('No point rewards match search.') : _('No point rewards.'));
	};
	const update = () => replaceChildren(results, renderRows());
	searchUpdate = update;
	update();
	return E('div', {}, [
		searchPanel(),
		pointLoading ? E('div', { 'class': 'alert-message' }, _('Loading point rewards...')) : '',
		pointError ? E('div', { 'class': 'alert-message warning' }, pointError) : '',
		!pointLoading && !pointError ? results : '',
		refreshButton()
	]);
}

function bookmarkPanel() {
	bookmarks = loadBookmarks();
	const results = E('div');
	const update = () => {
		bookmarks = loadBookmarks();
		replaceChildren(results, packageResultsNode(filterPackages(bookmarks), _('No bookmarked packages.')));
	};
	searchUpdate = update;
	update();
	return E('div', {}, [
		searchPanel(),
		results,
		refreshButton()
	]);
}

function refreshButton() {
	return E('button', { 'class': 'btn cbi-button cbi-button-reload', 'style': 'margin-top:1em', 'click': () => {
		if (storeMode === 'point')
			return pointFamilyCode ? fetchPointFamily(pointFamilyCode) : fetchPoint(true);
		if (storeMode === 'custom' && customView === 'family-list')
			return fetchFamilyList(true);
		if (storeMode === 'custom' && customFamilyCode)
			return fetchCustomFamily(customFamilyCode);
		window.location.reload();
	} }, _('Refresh'));
}

function shopHome(items) {
	const results = E('div');
	const update = () => replaceChildren(results, packageResultsNode(visibleStoreItems(shopData), _('No packages in this Store section.')));
	searchUpdate = update;
	replaceChildren(results, packageResultsNode(items, _('No packages in this Store section.')));
	return E('div', {}, [
		searchPanel(),
		results,
		refreshButton()
	]);
}

function renderShopContent() {
	if (!contentBox)
		return;
	if (storeMode === 'bookmark') {
		replaceChildren(contentBox, bookmarkPanel());
		return;
	}
	if (storeMode === 'custom') {
		replaceChildren(contentBox, customView === 'family-list' ? familyListPanel() : (customView === 'history' ? customHistoryPanel() : customOverviewPanel()));
		return;
	}
	if (storeMode === 'point') {
		replaceChildren(contentBox, pointPanel());
		return;
	}
	replaceChildren(contentBox, shopHome(visibleStoreItems(shopData)));
}

function setStoreScope(mode, view) {
	if (mode === 'point') {
		pointFamilyCode = '';
		pointFamilyResult = null;
		pointFamilyError = '';
		pointFamilyLoading = false;
	} else {
		pointFamilyCode = '';
		pointFamilyResult = null;
		pointFamilyError = '';
		pointFamilyLoading = false;
	}
	storeMode = mode === 'custom' || mode === 'bookmark' || mode === 'point' ? mode : 'main';
	customView = view === 'history' || view === 'family-list' ? view : 'overview';
}

function storeScopeFromRoute() {
	let path = '';
	if (L.env && Array.isArray(L.env.dispatchpath))
		path = L.env.dispatchpath.join('/');
	if (typeof window !== 'undefined' && window.location && window.location.pathname)
		path += '/' + window.location.pathname;
	path = path.toLowerCase();

	if (path.indexOf('/store/custom/family-list') >= 0 || path.indexOf('store/custom/family-list') === 0)
		return { mode: 'custom', view: 'family-list' };
	if (path.indexOf('/store/custom/history') >= 0 || path.indexOf('store/custom/history') === 0)
		return { mode: 'custom', view: 'history' };
	if (path.indexOf('/store/custom') >= 0 || path.indexOf('store/custom') === 0)
		return { mode: 'custom', view: 'overview' };
	if (path.indexOf('/store/point') >= 0 || path.indexOf('store/point') === 0)
		return { mode: 'point' };
	if (path.indexOf('/store/bookmark') >= 0 || path.indexOf('store/bookmark') === 0)
		return { mode: 'bookmark' };

	return { mode: 'main' };
}

function setStoreScopeFromRoute() {
	const scope = storeScopeFromRoute();
	setStoreScope(scope.mode, scope.view);
}

return view.extend({
	load() {
		setStoreScopeFromRoute();
		if (storeMode === 'custom' || storeMode === 'bookmark' || storeMode === 'point')
			return Promise.resolve({ ok: true, local: true });
		return callBitsxl([ 'json', 'shop' ]);
	},

	render(data) {
		resetCartPointerDrag(false);
		cartModalBox = null;
		setStoreScopeFromRoute();
		shopData = data || {};
		cart = loadCart();
		bookmarks = loadBookmarks();
		contentBox = E('div');
		cartFloatBox = E('div', {
			'style': 'position:fixed;right:.9rem;right:calc(.9rem + env(safe-area-inset-right, 0px));bottom:.9rem;bottom:calc(.9rem + env(safe-area-inset-bottom, 0px));z-index:850'
		});
		renderFloatingCart();
		window.setTimeout(renderShopContent, 0);
		return E('div', { 'class': 'cbi-map' }, [
			data && data.ok ? contentBox : E('div', { 'class': 'alert-message warning' }, data && data.error || _('Failed to load store.')),
			cartFloatBox
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
