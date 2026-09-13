'use strict';
'require view';
'require form';
'require uci';

const ENV_OPTIONS = [
	[ 'BASE_API_URL', _('Base API URL') ],
	[ 'BASE_CIAM_URL', _('Base CIAM URL') ],
	[ 'BASIC_AUTH', _('Basic Auth') ],
	[ 'AX_FP_KEY', _('AX Fingerprint Key') ],
	[ 'UA', _('User Agent') ],
	[ 'API_KEY', _('API Key') ],
	[ 'ENCRYPTED_FIELD_KEY', _('Encrypted Field Key') ],
	[ 'XDATA_KEY', _('XData Key') ],
	[ 'AX_API_SIG_KEY', _('AX API Signature Key') ],
	[ 'X_API_BASE_SECRET', _('X API Base Secret') ]
];

const DECOY_PAIRS = [
	[ 'DECOY_PREPAID_FAMILY_CODE', 'DECOY_PREPAID_PACKAGE_NUMBER', _('PREPAID'), _('Leave both fields blank to use the built-in prepaid decoy.') ],
	[ 'DECOY_PRIORITAS_FAMILY_CODE', 'DECOY_PRIORITAS_PACKAGE_NUMBER', _('PRIORITAS / GO'), _('Leave both fields blank to use the built-in PRIORITAS/GO decoy.') ],
	[ 'DECOY_PRIOHYBRID_FAMILY_CODE', 'DECOY_PRIOHYBRID_PACKAGE_NUMBER', _('PRIOHYBRID'), _('Leave both fields blank to use the PRIORITAS pair or built-in PRIORITAS decoy.') ]
];
const ENV_WARNING = _('!! Jangan ubah konfigurasi ini jika bukan profesional. Nilai salah dapat membuat login dan cek kuota gagal.');
const SOFT_LINE = 'rgba(127,127,127,.16)';
const SOFT_BORDER = 'linear-gradient(transparent,transparent) padding-box,linear-gradient(135deg,rgba(127,127,127,.26),rgba(127,127,127,.08),rgba(127,127,127,.20)) border-box';

function pageTitle(title) {
	const main = document.getElementById('maincontent');
	const tabmenu = document.getElementById('tabmenu');
	if (!main || !tabmenu || main.querySelector('.bitsxl-page-title'))
		return;
	main.insertBefore(E('div', { 'class': 'bitsxl-page-title' }, [
		E('h2', { 'name': 'content', 'style': 'margin:0 0 .35em' }, title),
		E('div', { 'style': 'border-top:1px solid ' + SOFT_LINE + ';margin:0 0 1em' })
	]), tabmenu);
}

function utf8Length(value) {
	try {
		return encodeURIComponent(value).replace(/%[0-9A-F]{2}/gi, 'x').length;
	} catch (err) {
		return Number.POSITIVE_INFINITY;
	}
}

function siblingFormValue(option, sectionId, name) {
	const match = option.map.lookupOption(name, sectionId);
	if (!match || !match[0])
		return '';
	return String(match[0].formvalue(match[1] != null ? match[1] : sectionId) || '');
}

function validateDecoyPair(peerName, kind) {
	return function(sectionId, value) {
		value = String(value || '');
		const peerValue = siblingFormValue(this, sectionId, peerName);
		if (!!value !== !!peerValue)
			return _('Family code and package number must be filled in together, or both left blank.');
		if (!value)
			return true;
		if (kind === 'family') {
			if (utf8Length(value) > 128 || /[\s\x00-\x1f\x7f]/.test(value))
				return _('Family code must be 1-128 bytes and contain no whitespace or control characters.');
		} else if (!/^[0-9]{1,10}$/.test(value) || Number(value) < 1 || Number(value) > 2147483647) {
			return _('Package number must be a decimal integer from 1 to 2147483647.');
		}
		return true;
	};
}

function groupDecoyCards(root) {
	if (!root || !root.querySelector)
		return root;

	DECOY_PAIRS.forEach((item) => {
		const family = root.querySelector('[data-name="' + item[0] + '"]');
		const number = root.querySelector('[data-name="' + item[1] + '"]');
		if (!family || !number || family.parentNode !== number.parentNode)
			return;

		const parent = family.parentNode;
		const card = E('div', {
			'class': 'cbi-section',
			'data-bitsxl-decoy-card': item[0],
			'style': 'margin:.9em 0;padding:clamp(.85em,2vw,1.1em);border:1px solid transparent;border-radius:8px;background:' + SOFT_BORDER
		}, [
			E('div', { 'style': 'padding-bottom:.7em;margin-bottom:.15em;border-bottom:1px solid ' + SOFT_LINE }, [
				E('h3', { 'style': 'margin:0;line-height:1.25' }, item[2]),
				E('div', { 'class': 'cbi-section-descr', 'style': 'margin:.3em 0 0' }, item[3])
			])
		]);

		parent.insertBefore(card, family);
		card.appendChild(family);
		card.appendChild(number);
	});

	return root;
}

return view.extend({
	load() {
		return uci.load('bitsxl');
	},

	render() {
		pageTitle(_('Settings'));
		let m, s, o;

		m = new form.Map('bitsxl', _('BITS XL Settings'));
		s = m.section(form.NamedSection, 'config', 'bitsxl');
		s.anonymous = true;
		s.tab('env', _('Environment'), ENV_WARNING);
		s.tab('decoy', _('Decoy'), _('Optional family and package number overrides shared by Balance + Decoy and Balance + Decoy V2. Each pair must be completed together; leave both fields blank to use built-in defaults.'));

		ENV_OPTIONS.forEach((item) => {
			o = s.taboption('env', form.Value, item[0], item[1]);
			o.rmempty = false;
			o.datatype = 'string';
			o.placeholder = item[0];
		});

		DECOY_PAIRS.forEach((item) => {
			const family = s.taboption('decoy', form.Value, item[0], _('%s — Family Code').format(item[2]));
			family.rmempty = true;
			family.placeholder = _('Enter family code');
			family.validate = validateDecoyPair(item[1], 'family');

			const number = s.taboption('decoy', form.Value, item[1], _('%s — Nomor Paket').format(item[2]));
			number.rmempty = true;
			number.placeholder = '1';
			number.description = _('One-based position after flattening package variants and their options in API order; this is not the option order field.');
			number.validate = validateDecoyPair(item[0], 'number');
		});

		return Promise.resolve(m.render()).then(groupDecoyCards);
	}
});
