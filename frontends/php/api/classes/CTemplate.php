<?php
/*
** Zabbix
** Copyright (C) 2000-2011 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/


/**
 * @package API
 */
class CTemplate extends CZBXAPI {

	protected $tableName = 'hosts';

	protected $tableAlias = 'h';

	/**
	 * Get Template data
	 *
	 * @param array $options
	 * @return array|boolean Template data as array or false if error
	 */
	public function get($options = array()) {
		$result = array();
		$nodeCheck = false;
		$userType = self::$userData['type'];
		$userid = self::$userData['userid'];

		// allowed columns for sorting
		$sortColumns = array('hostid', 'host', 'name');

		// allowed output options for [ select_* ] params
		$subselectsAllowedOutputs = array(API_OUTPUT_REFER, API_OUTPUT_EXTEND);

		$sqlParts = array(
			'select'	=> array('templates' => 'h.hostid'),
			'from'		=> array('hosts' => 'hosts h'),
			'where'		=> array('h.status='.HOST_STATUS_TEMPLATE),
			'group'		=> array(),
			'order'		=> array(),
			'limit'		=> null
		);

		$defOptions = array(
			'nodeids'					=> null,
			'groupids'					=> null,
			'templateids'				=> null,
			'parentTemplateids'			=> null,
			'hostids'					=> null,
			'graphids'					=> null,
			'itemids'					=> null,
			'triggerids'				=> null,
			'with_items'				=> null,
			'with_triggers'				=> null,
			'with_graphs'				=> null,
			'editable' 					=> null,
			'nopermissions'				=> null,
			// filter
			'filter'					=> null,
			'search'					=> '',
			'searchByAny'				=> null,
			'startSearch'				=> null,
			'excludeSearch'				=> null,
			'searchWildcardsEnabled'	=> null,
			// output
			'output'					=> API_OUTPUT_REFER,
			'selectGroups'				=> null,
			'selectHosts'				=> null,
			'selectTemplates'			=> null,
			'selectParentTemplates'		=> null,
			'selectItems'				=> null,
			'selectDiscoveries'			=> null,
			'selectTriggers'			=> null,
			'selectGraphs'				=> null,
			'selectApplications'		=> null,
			'selectMacros'				=> null,
			'selectScreens'				=> null,
			'countOutput'				=> null,
			'groupCount'				=> null,
			'preservekeys'				=> null,
			'sortfield'					=> '',
			'sortorder'					=> '',
			'limit'						=> null,
			'limitSelects'				=> null
		);
		$options = zbx_array_merge($defOptions, $options);

		if (is_array($options['output'])) {
			unset($sqlParts['select']['templates']);

			$dbTable = DB::getSchema('hosts');
			$sqlParts['select']['hostid'] = 'h.hostid';
			foreach ($options['output'] as $field) {
				if ($field == 'templateid') {
					continue;
				}
				if (isset($dbTable['fields'][$field])) {
					$sqlParts['select'][$field] = 'h.'.$field;
				}
			}
			$options['output'] = API_OUTPUT_CUSTOM;
		}

		// editable + PERMISSION CHECK
		if ((USER_TYPE_SUPER_ADMIN == $userType) || $options['nopermissions']) {
		}
		else{
			$permission = $options['editable']?PERM_READ_WRITE:PERM_READ_ONLY;

			$sqlParts['from']['hosts_groups'] = 'hosts_groups hg';
			$sqlParts['from']['rights'] = 'rights r';
			$sqlParts['from']['users_groups'] = 'users_groups ug';
			$sqlParts['where'][] = 'hg.hostid=h.hostid';
			$sqlParts['where'][] = 'r.id=hg.groupid ';
			$sqlParts['where'][] = 'r.groupid=ug.usrgrpid';
			$sqlParts['where'][] = 'ug.userid='.$userid;
			$sqlParts['where'][] = 'r.permission>='.$permission;
			$sqlParts['where'][] = 'NOT EXISTS('.
				'SELECT hgg.groupid'.
				' FROM hosts_groups hgg,rights rr,users_groups gg'.
				' WHERE hgg.hostid=hg.hostid'.
				' AND rr.id=hgg.groupid'.
				' AND rr.groupid=gg.usrgrpid'.
				' AND gg.userid='.$userid.
				' AND rr.permission<'.$permission.')';
		}

		// nodeids
		$nodeids = !is_null($options['nodeids']) ? $options['nodeids'] : get_current_nodeid();

		// groupids
		if (!is_null($options['groupids'])) {
			zbx_value2array($options['groupids']);

			if ($options['output'] != API_OUTPUT_SHORTEN) {
				$sqlParts['select']['groupid'] = 'hg.groupid';
			}

			$sqlParts['from']['hosts_groups'] = 'hosts_groups hg';
			$sqlParts['where'][] = DBcondition('hg.groupid', $options['groupids']);
			$sqlParts['where']['hgh'] = 'hg.hostid=h.hostid';

			if (!is_null($options['groupCount'])) {
				$sqlParts['group']['hg'] = 'hg.groupid';
			}

			if (!$nodeCheck) {
				$nodeCheck = true;
				$sqlParts['where'][] = DBin_node('hg.groupid', $nodeids);
			}
		}

		// templateids
		if (!is_null($options['templateids'])) {
			zbx_value2array($options['templateids']);

			$sqlParts['where']['templateid'] = DBcondition('h.hostid', $options['templateids']);

			if (!$nodeCheck) {
				$nodeCheck = true;
				$sqlParts['where'][] = DBin_node('h.hostid', $nodeids);
			}
		}

		// parentTemplateids
		if (!is_null($options['parentTemplateids'])) {
			zbx_value2array($options['parentTemplateids']);
			if ($options['output'] != API_OUTPUT_SHORTEN) {
				$sqlParts['select']['parentTemplateid'] = 'ht.templateid as parentTemplateid';
			}

			$sqlParts['from']['hosts_templates'] = 'hosts_templates ht';
			$sqlParts['where'][] = DBcondition('ht.templateid', $options['parentTemplateids']);
			$sqlParts['where']['hht'] = 'h.hostid=ht.hostid';

			if (!is_null($options['groupCount'])) {
				$sqlParts['group']['templateid'] = 'ht.templateid';
			}

			if (!$nodeCheck) {
				$nodeCheck = true;
				$sqlParts['where'][] = DBin_node('ht.templateid', $nodeids);
			}
		}

		// hostids
		if (!is_null($options['hostids'])) {
			zbx_value2array($options['hostids']);

			if ($options['output'] != API_OUTPUT_SHORTEN) {
				$sqlParts['select']['linked_hostid'] = 'ht.hostid as linked_hostid';
			}

			$sqlParts['from']['hosts_templates'] = 'hosts_templates ht';
			$sqlParts['where'][] = DBcondition('ht.hostid', $options['hostids']);
			$sqlParts['where']['hht'] = 'h.hostid=ht.templateid';

			if (!is_null($options['groupCount'])) {
				$sqlParts['group']['ht'] = 'ht.hostid';
			}

			if (!$nodeCheck) {
				$nodeCheck = true;
				$sqlParts['where'][] = DBin_node('ht.hostid', $nodeids);
			}
		}

		// itemids
		if (!is_null($options['itemids'])) {
			zbx_value2array($options['itemids']);

			if ($options['output'] != API_OUTPUT_SHORTEN) {
				$sqlParts['select']['itemid'] = 'i.itemid';
			}

			$sqlParts['from']['items'] = 'items i';
			$sqlParts['where'][] = DBcondition('i.itemid', $options['itemids']);
			$sqlParts['where']['hi'] = 'h.hostid=i.hostid';

			if (!$nodeCheck) {
				$nodeCheck = true;
				$sqlParts['where'][] = DBin_node('i.itemid', $nodeids);
			}
		}

		// triggerids
		if (!is_null($options['triggerids'])) {
			zbx_value2array($options['triggerids']);
			if ($options['output'] != API_OUTPUT_SHORTEN) {
				$sqlParts['select']['triggerid'] = 'f.triggerid';
			}

			$sqlParts['from']['functions'] = 'functions f';
			$sqlParts['from']['items'] = 'items i';
			$sqlParts['where'][] = DBcondition('f.triggerid', $options['triggerids']);
			$sqlParts['where']['hi'] = 'h.hostid=i.hostid';
			$sqlParts['where']['fi'] = 'f.itemid=i.itemid';

			if (!$nodeCheck) {
				$nodeCheck = true;
				$sqlParts['where'][] = DBin_node('f.triggerid', $nodeids);
			}
		}

		// graphids
		if (!is_null($options['graphids'])) {
			zbx_value2array($options['graphids']);

			if ($options['output'] != API_OUTPUT_SHORTEN) {
				$sqlParts['select']['graphid'] = 'gi.graphid';
			}

			$sqlParts['from']['graphs_items'] = 'graphs_items gi';
			$sqlParts['from']['items'] = 'items i';
			$sqlParts['where'][] = DBcondition('gi.graphid', $options['graphids']);
			$sqlParts['where']['igi'] = 'i.itemid=gi.itemid';
			$sqlParts['where']['hi'] = 'h.hostid=i.hostid';

			if (!$nodeCheck) {
				$nodeCheck = true;
				$sqlParts['where'][] = DBin_node('gi.graphid', $nodeids);
			}
		}

		// node check !!!!
		// should last, after all ****IDS checks
		if (!$nodeCheck) {
			$sqlParts['where'][] = DBin_node('h.hostid', $nodeids);
		}

		// with_items
		if (!is_null($options['with_items'])) {
			$sqlParts['where'][] = 'EXISTS (SELECT i.hostid FROM items i WHERE h.hostid=i.hostid )';
		}

		// with_triggers
		if (!is_null($options['with_triggers'])) {
			$sqlParts['where'][] = 'EXISTS('.
				'SELECT i.itemid'.
				' FROM items i,functions f,triggers t'.
				' WHERE i.hostid=h.hostid'.
				' AND i.itemid=f.itemid'.
				' AND f.triggerid=t.triggerid)';
		}

		// with_graphs
		if (!is_null($options['with_graphs'])) {
			$sqlParts['where'][] = 'EXISTS('.
				'SELECT DISTINCT i.itemid'.
				' FROM items i,graphs_items gi'.
				' WHERE i.hostid=h.hostid'.
				' AND i.itemid=gi.itemid)';
		}

		// filter
		if (is_array($options['filter'])) {
			zbx_db_filter('hosts h', $options, $sqlParts);
		}

		// search
		if (is_array($options['search'])) {
			zbx_db_search('hosts h', $options, $sqlParts);
		}

		// output
		if ($options['output'] == API_OUTPUT_EXTEND) {
			$sqlParts['select']['templates'] = 'h.*';
		}

		// countOutput
		if (!is_null($options['countOutput'])) {
			$options['sortfield'] = '';
			$sqlParts['select'] = array('count(DISTINCT h.hostid) as rowscount');

			// groupCount
			if (!is_null($options['groupCount'])) {
				foreach ($sqlParts['group'] as $key => $fields) {
					$sqlParts['select'][$key] = $fields;
				}
			}
		}

		// sorting
		zbx_db_sorting($sqlParts, $options, $sortColumns, 'h');

		// limit
		if (zbx_ctype_digit($options['limit']) && $options['limit']) {
			$sqlParts['limit'] = $options['limit'];
		}
		//-------------

		$templateids = array();

		$res = DBselect($this->createSelectQueryFromParts($sqlParts), $sqlParts['limit']);
		while ($template = DBfetch($res)) {
			if (!is_null($options['countOutput'])) {
				if (!is_null($options['groupCount']))
					$result[] = $template;
				else
					$result = $template['rowscount'];
			}
			else{
				$template['templateid'] = $template['hostid'];
				$templateids[$template['templateid']] = $template['templateid'];

				if ($options['output'] == API_OUTPUT_SHORTEN) {
					$result[$template['templateid']] = array('templateid' => $template['templateid']);
				}
				else{
					if (!isset($result[$template['templateid']])) $result[$template['templateid']]= array();

					if (!is_null($options['selectGroups']) && !isset($result[$template['templateid']]['groups'])) {
						$template['groups'] = array();
					}
					if (!is_null($options['selectTemplates']) && !isset($result[$template['templateid']]['templates'])) {
						$template['templates'] = array();
					}
					if (!is_null($options['selectHosts']) && !isset($result[$template['templateid']]['hosts'])) {
						$template['hosts'] = array();
					}
					if (!is_null($options['selectParentTemplates']) && !isset($result[$template['templateid']]['parentTemplates'])) {
						$template['parentTemplates'] = array();
					}
					if (!is_null($options['selectItems']) && !isset($result[$template['templateid']]['items'])) {
						$template['items'] = array();
					}
					if (!is_null($options['selectDiscoveries']) && !isset($result[$template['hostid']]['discoveries'])) {
						$result[$template['hostid']]['discoveries'] = array();
					}
					if (!is_null($options['selectTriggers']) && !isset($result[$template['templateid']]['triggers'])) {
						$template['triggers'] = array();
					}
					if (!is_null($options['selectGraphs']) && !isset($result[$template['templateid']]['graphs'])) {
						$template['graphs'] = array();
					}
					if (!is_null($options['selectApplications']) && !isset($result[$template['templateid']]['applications'])) {
						$template['applications'] = array();
					}
					if (!is_null($options['selectMacros']) && !isset($result[$template['templateid']]['macros'])) {
						$template['macros'] = array();
					}
					if (!is_null($options['selectScreens']) && !isset($result[$template['templateid']]['screens'])) {
						$template['screens'] = array();
					}

					// groupids
					if (isset($template['groupid']) && is_null($options['selectGroups'])) {
						if (!isset($result[$template['templateid']]['groups']))
							$result[$template['templateid']]['groups'] = array();

						$result[$template['templateid']]['groups'][] = array('groupid' => $template['groupid']);
						unset($template['groupid']);
					}

					// hostids
					if (isset($template['linked_hostid']) && is_null($options['selectHosts'])) {
						if (!isset($result[$template['templateid']]['hosts']))
							$result[$template['templateid']]['hosts'] = array();

						$result[$template['templateid']]['hosts'][] = array('hostid' => $template['linked_hostid']);
						unset($template['linked_hostid']);
					}
					// parentTemplateids
					if (isset($template['parentTemplateid']) && is_null($options['selectParentTemplates'])) {
						if (!isset($result[$template['templateid']]['parentTemplates']))
							$result[$template['templateid']]['parentTemplates'] = array();

						$result[$template['templateid']]['parentTemplates'][] = array('templateid' => $template['parentTemplateid']);
						unset($template['parentTemplateid']);
					}

					// itemids
					if (isset($template['itemid']) && is_null($options['selectItems'])) {
						if (!isset($result[$template['templateid']]['items']))
							$result[$template['templateid']]['items'] = array();

						$result[$template['templateid']]['items'][] = array('itemid' => $template['itemid']);
						unset($template['itemid']);
					}

					// triggerids
					if (isset($template['triggerid']) && is_null($options['selectTriggers'])) {
						if (!isset($result[$template['hostid']]['triggers']))
							$result[$template['hostid']]['triggers'] = array();

						$result[$template['hostid']]['triggers'][] = array('triggerid' => $template['triggerid']);
						unset($template['triggerid']);
					}

					// graphids
					if (isset($template['graphid']) && is_null($options['selectGraphs'])) {
						if (!isset($result[$template['templateid']]['graphs'])) $result[$template['templateid']]['graphs'] = array();

						$result[$template['templateid']]['graphs'][] = array('graphid' => $template['graphid']);
						unset($template['graphid']);
					}

					$result[$template['templateid']] += $template;
				}
			}

		}

		if (!is_null($options['countOutput'])) {
			return $result;
		}

		// Adding Objects
		// Adding Groups
		if (!is_null($options['selectGroups']) && str_in_array($options['selectGroups'], $subselectsAllowedOutputs)) {
			$objParams = array(
				'nodeids' => $nodeids,
				'output' => $options['selectGroups'],
				'hostids' => $templateids,
				'preservekeys' => 1
			);
			$groups = API::HostGroup()->get($objParams);
			foreach ($groups as $groupid => $group) {
				$ghosts = $group['hosts'];
				unset($group['hosts']);
				foreach ($ghosts as $hnum => $template) {
					$result[$template['hostid']]['groups'][] = $group;
				}
			}
		}

		// Adding Templates
		if (!is_null($options['selectTemplates'])) {
			$objParams = array(
				'nodeids' => $nodeids,
				'parentTemplateids' => $templateids,
				'preservekeys' => 1
			);

			if (is_array($options['selectTemplates']) || str_in_array($options['selectTemplates'], $subselectsAllowedOutputs)) {
				$objParams['output'] = $options['selectTemplates'];
				$templates = API::Template()->get($objParams);

				if (!is_null($options['limitSelects'])) order_result($templates, 'host');
				foreach ($templates as $templateid => $template) {
					unset($templates[$templateid]['parentTemplates']);

					if (isset($template['parentTemplates']) && is_array($template['parentTemplates'])) {
						$count = array();
						foreach ($template['parentTemplates'] as $parentTemplate) {
							if (!is_null($options['limitSelects'])) {
								if (!isset($count[$parentTemplate['templateid']])) $count[$parentTemplate['templateid']] = 0;
								$count[$parentTemplate['hostid']]++;

								if ($count[$parentTemplate['templateid']] > $options['limitSelects']) continue;
							}

							$result[$parentTemplate['templateid']]['templates'][] = &$templates[$templateid];
						}
					}
				}
			}
			elseif (API_OUTPUT_COUNT == $options['selectTemplates']) {
				$objParams['countOutput'] = 1;
				$objParams['groupCount'] = 1;

				$templates = API::Template()->get($objParams);
				$templates = zbx_toHash($templates, 'hostid');
				foreach ($result as $templateid => $template) {
					if (isset($templates[$groupid]))
						$result[$templateid]['templates'] = $templates[$templateid]['rowscount'];
					else
						$result[$templateid]['templates'] = 0;
				}
			}
		}

		// Adding Hosts
		if (!is_null($options['selectHosts'])) {
			$objParams = array(
				'nodeids' => $nodeids,
				'templateids' => $templateids,
				'preservekeys' => 1
			);

			if (is_array($options['selectHosts']) || str_in_array($options['selectHosts'], $subselectsAllowedOutputs)) {
				$objParams['output'] = $options['selectHosts'];
				$hosts = API::Host()->get($objParams);

				if (!is_null($options['limitSelects'])) order_result($hosts, 'host');
				foreach ($hosts as $hostid => $host) {
					unset($hosts[$hostid]['templates']);

					foreach ($host['templates'] as $tnum => $template) {
						if (!is_null($options['limitSelects'])) {
							if (!isset($count[$template['templateid']])) $count[$template['templateid']] = 0;
							$count[$template['templateid']]++;

							if ($count[$template['templateid']] > $options['limitSelects']) continue;
						}

						$result[$template['templateid']]['hosts'][] = &$hosts[$hostid];
					}
				}
			}
			elseif (API_OUTPUT_COUNT == $options['selectHosts']) {
				$objParams['countOutput'] = 1;
				$objParams['groupCount'] = 1;

				$hosts = API::Host()->get($objParams);
				$hosts = zbx_toHash($hosts, 'hostid');
				foreach ($result as $templateid => $template) {
					if (isset($hosts[$templateid]))
						$result[$templateid]['hosts'] = $hosts[$templateid]['rowscount'];
					else
						$result[$templateid]['hosts'] = 0;
				}
			}
		}

		// Adding parentTemplates
		if (!is_null($options['selectParentTemplates'])) {
			$objParams = array(
				'nodeids' => $nodeids,
				'hostids' => $templateids,
				'preservekeys' => 1
			);

			if (is_array($options['selectParentTemplates']) || str_in_array($options['selectParentTemplates'], $subselectsAllowedOutputs)) {
				$objParams['output'] = $options['selectParentTemplates'];
				$templates = API::Template()->get($objParams);

				if (!is_null($options['limitSelects'])) order_result($templates, 'host');
				foreach ($templates as $templateid => $template) {
					unset($templates[$templateid]['hosts']);

					foreach ($template['hosts'] as $hnum => $host) {
						if (!is_null($options['limitSelects'])) {
							if (!isset($count[$host['hostid']])) $count[$host['hostid']] = 0;
							$count[$host['hostid']]++;

							if ($count[$host['hostid']] > $options['limitSelects']) continue;
						}

						$result[$host['hostid']]['parentTemplates'][] = &$templates[$templateid];
					}
				}
			}
			elseif (API_OUTPUT_COUNT == $options['selectTemplates']) {
				$objParams['countOutput'] = 1;
				$objParams['groupCount'] = 1;

				$templates = API::Template()->get($objParams);
				$templates = zbx_toHash($templates, 'hostid');
				foreach ($result as $templateid => $template) {
					if (isset($templates[$groupid]))
						$result[$templateid]['parentTemplates'] = $templates[$templateid]['rowscount'];
					else
						$result[$templateid]['parentTemplates'] = 0;
				}
			}
		}

		// Adding Items
		if (!is_null($options['selectItems'])) {
			$objParams = array(
				'nodeids' => $nodeids,
				'hostids' => $templateids,
				'filter' => array('flags' => array(ZBX_FLAG_DISCOVERY_NORMAL, ZBX_FLAG_DISCOVERY_CREATED)),
				'nopermissions' => 1,
				'preservekeys' => 1
			);

			if (is_array($options['selectItems']) || str_in_array($options['selectItems'], $subselectsAllowedOutputs)) {
				$objParams['output'] = $options['selectItems'];
				$items = API::Item()->get($objParams);

				if (!is_null($options['limitSelects'])) order_result($items, 'name');

				$count = array();
				foreach ($items as $itemid => $item) {
					if (!is_null($options['limitSelects'])) {
						if (!isset($count[$item['hostid']])) $count[$item['hostid']] = 0;
						$count[$item['hostid']]++;

						if ($count[$item['hostid']] > $options['limitSelects']) continue;
					}

					$result[$item['hostid']]['items'][] = &$items[$itemid];
				}
			}
			elseif (API_OUTPUT_COUNT == $options['selectItems']) {
				$objParams['countOutput'] = 1;
				$objParams['groupCount'] = 1;

				$items = API::Item()->get($objParams);
				$items = zbx_toHash($items, 'hostid');
				foreach ($result as $templateid => $template) {
					if (isset($items[$templateid]))
						$result[$templateid]['items'] = $items[$templateid]['rowscount'];
					else
						$result[$templateid]['items'] = 0;
				}
			}
		}

		// Adding Discoveries
		if (!is_null($options['selectDiscoveries'])) {
			$objParams = array(
				'nodeids' => $nodeids,
				'hostids' => $templateids,
				'filter' => array('flags' => ZBX_FLAG_DISCOVERY),
				'nopermissions' => 1,
				'preservekeys' => 1,
			);

			if (is_array($options['selectDiscoveries']) || str_in_array($options['selectDiscoveries'], $subselectsAllowedOutputs)) {
				$objParams['output'] = $options['selectDiscoveries'];
				$items = API::Item()->get($objParams);

				if (!is_null($options['limitSelects'])) order_result($items, 'name');
				foreach ($items as $itemid => $item) {
					unset($items[$itemid]['hosts']);
					foreach ($item['hosts'] as $hnum => $host) {
						if (!is_null($options['limitSelects'])) {
							if (!isset($count[$host['hostid']])) $count[$host['hostid']] = 0;
							$count[$host['hostid']]++;

							if ($count[$host['hostid']] > $options['limitSelects']) continue;
						}

						$result[$host['hostid']]['discoveries'][] = &$items[$itemid];
					}
				}
			}
			elseif (API_OUTPUT_COUNT == $options['selectDiscoveries']) {
				$objParams['countOutput'] = 1;
				$objParams['groupCount'] = 1;

				$items = API::Item()->get($objParams);
				$items = zbx_toHash($items, 'hostid');
				foreach ($result as $hostid => $host) {
					if (isset($items[$hostid]))
						$result[$hostid]['discoveries'] = $items[$hostid]['rowscount'];
					else
						$result[$hostid]['discoveries'] = 0;
				}
			}
		}

		// Adding triggers
		if (!is_null($options['selectTriggers'])) {
			$objParams = array(
				'nodeids' => $nodeids,
				'hostids' => $templateids,
				'nopermissions' => 1,
				'preservekeys' => 1
			);

			if (is_array($options['selectTriggers']) || str_in_array($options['selectTriggers'], $subselectsAllowedOutputs)) {
				$objParams['output'] = $options['selectTriggers'];
				$triggers = API::Trigger()->get($objParams);

				if (!is_null($options['limitSelects'])) order_result($triggers, 'description');
				foreach ($triggers as $triggerid => $trigger) {
					unset($triggers[$triggerid]['hosts']);

					foreach ($trigger['hosts'] as $hnum => $host) {
						if (!is_null($options['limitSelects'])) {
							if (!isset($count[$host['hostid']])) $count[$host['hostid']] = 0;
							$count[$host['hostid']]++;

							if ($count[$host['hostid']] > $options['limitSelects']) continue;
						}

						$result[$host['hostid']]['triggers'][] = &$triggers[$triggerid];
					}
				}
			}
			elseif (API_OUTPUT_COUNT == $options['selectTriggers']) {
				$objParams['countOutput'] = 1;
				$objParams['groupCount'] = 1;

				$triggers = API::Trigger()->get($objParams);
				$triggers = zbx_toHash($triggers, 'hostid');
				foreach ($result as $templateid => $template) {
					if (isset($triggers[$templateid]))
						$result[$templateid]['triggers'] = $triggers[$templateid]['rowscount'];
					else
						$result[$templateid]['triggers'] = 0;
				}
			}
		}

		// Adding graphs
		if (!is_null($options['selectGraphs'])) {
			$objParams = array(
				'nodeids' => $nodeids,
				'hostids' => $templateids,
				'nopermissions' => 1,
				'preservekeys' => 1
			);

			if (is_array($options['selectGraphs']) || str_in_array($options['selectGraphs'], $subselectsAllowedOutputs)) {
				$objParams['output'] = $options['selectGraphs'];
				$graphs = API::Graph()->get($objParams);

				if (!is_null($options['limitSelects'])) order_result($graphs, 'name');
				foreach ($graphs as $graphid => $graph) {
					unset($graphs[$graphid]['hosts']);

					foreach ($graph['hosts'] as $hnum => $host) {
						if (!is_null($options['limitSelects'])) {
							if (!isset($count[$host['hostid']])) $count[$host['hostid']] = 0;
							$count[$host['hostid']]++;

							if ($count[$host['hostid']] > $options['limitSelects']) continue;
						}

						$result[$host['hostid']]['graphs'][] = &$graphs[$graphid];
					}
				}
			}
			elseif (API_OUTPUT_COUNT == $options['selectGraphs']) {
				$objParams['countOutput'] = 1;
				$objParams['groupCount'] = 1;

				$graphs = API::Graph()->get($objParams);
				$graphs = zbx_toHash($graphs, 'hostid');
				foreach ($result as $templateid => $template) {
					if (isset($graphs[$templateid]))
						$result[$templateid]['graphs'] = $graphs[$templateid]['rowscount'];
					else
						$result[$templateid]['graphs'] = 0;
				}
			}
		}

		// Adding applications
		if (!is_null($options['selectApplications'])) {
			$objParams = array(
				'nodeids' => $nodeids,
				'hostids' => $templateids,
				'nopermissions' => 1,
				'preservekeys' => 1
			);

			if (is_array($options['selectApplications']) || str_in_array($options['selectApplications'], $subselectsAllowedOutputs)) {
				$objParams['output'] = $options['selectApplications'];
				$applications = API::Application()->get($objParams);

				if (!is_null($options['limitSelects'])) order_result($applications, 'name');
				foreach ($applications as $applicationid => $application) {
					unset($applications[$applicationid]['hosts']);

					foreach ($application['hosts'] as $hnum => $host) {
						if (!is_null($options['limitSelects'])) {
							if (!isset($count[$host['hostid']])) $count[$host['hostid']] = 0;
							$count[$host['hostid']]++;

							if ($count[$host['hostid']] > $options['limitSelects']) continue;
						}

						$result[$host['hostid']]['applications'][] = &$applications[$applicationid];
					}
				}
			}
			elseif (API_OUTPUT_COUNT == $options['selectApplications']) {
				$objParams['countOutput'] = 1;
				$objParams['groupCount'] = 1;

				$applications = API::Application()->get($objParams);
				$applications = zbx_toHash($applications, 'hostid');
				foreach ($result as $templateid => $template) {
					if (isset($applications[$templateid]))
						$result[$templateid]['applications'] = $applications[$templateid]['rowscount'];
					else
						$result[$templateid]['applications'] = 0;
				}
			}
		}

		// Adding screens
		if (!is_null($options['selectScreens'])) {
			$objParams = array(
				'nodeids' => $nodeids,
				'templateids' => $templateids,
				'editable' => $options['editable'],
				'nopermissions' => 1,
				'preservekeys' => 1
			);

			if (is_array($options['selectScreens']) || str_in_array($options['selectScreens'], $subselectsAllowedOutputs)) {
				$objParams['output'] = $options['selectScreens'];

				$screens = API::TemplateScreen()->get($objParams);
				if (!is_null($options['limitSelects'])) order_result($screens, 'name');

				foreach ($screens as $screenid => $screen) {
					if (!is_null($options['limitSelects'])) {
						if (count($result[$screen['hostid']]['screens']) >= $options['limitSelects']) continue;
					}

					unset($screens[$screenid]['templates']);
					$result[$screen['hostid']]['screens'][] = &$screens[$screenid];
				}
			}
			elseif (API_OUTPUT_COUNT == $options['selectScreens']) {
				$objParams['countOutput'] = 1;
				$objParams['groupCount'] = 1;

				$screens = API::TemplateScreen()->get($objParams);
				$screens = zbx_toHash($screens, 'hostid');
				foreach ($result as $templateid => $template) {
					if (isset($screens[$templateid]))
						$result[$templateid]['screens'] = $screens[$templateid]['rowscount'];
					else
						$result[$templateid]['screens'] = 0;
				}
			}
		}

		// Adding macros
		if (!is_null($options['selectMacros']) && str_in_array($options['selectMacros'], $subselectsAllowedOutputs)) {
			$objParams = array(
				'nodeids' => $nodeids,
				'output' => $options['selectMacros'],
				'hostids' => $templateids,
				'preservekeys' => 1
			);
			$macros = API::UserMacro()->get($objParams);
			foreach ($macros as $macroid => $macro) {
				unset($macros[$macroid]['hosts']);

				foreach ($macro['hosts'] as $hnum => $host) {
					$result[$host['hostid']]['macros'][] = $macros[$macroid];
				}
			}
		}

		// removing keys (hash -> array)
		if (is_null($options['preservekeys'])) {
			$result = zbx_cleanHashes($result);
		}

		return $result;
	}

	/**
	 * Get Template ID by Template name
	 *
	 * @param array $template_data
	 * @param array $template_data['host']
	 * @param array $template_data['templateid']
	 * @return string templateid
	 */
	public function getObjects($templateData) {
		$options = array(
			'filter' => $templateData,
			'output'=>API_OUTPUT_EXTEND
		);

		if (isset($templateData['node']))
			$options['nodeids'] = getNodeIdByNodeName($templateData['node']);
		elseif (isset($templateData['nodeids']))
			$options['nodeids'] = $templateData['nodeids'];

		$result = $this->get($options);

		return $result;
	}

	public function exists($object) {
		$keyFields = array(array('templateid', 'host', 'name'));

		$options = array(
			'filter' => zbx_array_mintersect($keyFields, $object),
			'output' => API_OUTPUT_SHORTEN,
			'nopermissions' => 1,
			'limit' => 1
		);
		if (isset($object['node']))
			$options['nodeids'] = getNodeIdByNodeName($object['node']);
		elseif (isset($object['nodeids']))
			$options['nodeids'] = $object['nodeids'];

		$objs = $this->get($options);

		return !empty($objs);
	}

	/**
	 * Add Template
	 *
	 * @param array $templates multidimensional array with templates data
	 * @param string $templates['host']
	 * @return boolean
	 */
	public function create($templates) {
		$templates = zbx_toArray($templates);
		$templateids = array();

		// CHECK IF HOSTS HAVE AT LEAST 1 GROUP {{{
		foreach ($templates as $tnum => $template) {
			if (empty($template['groups'])) {
				self::exception(ZBX_API_ERROR_PARAMETERS, _s('No groups for template [ %s ]', $template['host']));
			}
			$templates[$tnum]['groups'] = zbx_toArray($templates[$tnum]['groups']);

			foreach ($templates[$tnum]['groups'] as $gnum => $group) {
				$groupids[$group['groupid']] = $group['groupid'];
			}
		}
		// }}} CHECK IF HOSTS HAVE AT LEAST 1 GROUP


		// PERMISSIONS {{{
		$options = array(
			'groupids' => $groupids,
			'editable' => 1,
			'preservekeys' => 1
		);
		$updGroups = API::HostGroup()->get($options);
		foreach ($groupids as $gnum => $groupid) {
			if (!isset($updGroups[$groupid])) {
				self::exception(ZBX_API_ERROR_PERMISSIONS, _('You do not have permission to perform this operation.'));
			}
		}
		// }}} PERMISSIONS

		foreach ($templates as $tnum => $template) {
			// If visible name is not given or empty it should be set to host name
			if (!isset($template['name']) || (isset($template['name']) && zbx_empty(trim($template['name']))))
			{
				if (isset($template['host'])) $template['name'] = $template['host'];
			}

			$templateDbFields = array(
				'host' => null
			);

			if (!check_db_fields($templateDbFields, $template)) {
				self::exception(ZBX_API_ERROR_PARAMETERS, _s('Field "host" is mandatory'));
			}

			if (!preg_match('/^'.ZBX_PREG_HOST_FORMAT.'$/', $template['host'])) {
				self::exception(ZBX_API_ERROR_PARAMETERS, _s('Incorrect characters used for Template name [ %1$s ]', $template['host']));
			}

			if (isset($template['host'])) {
				if ($this->exists(array('host' => $template['host']))) {
					self::exception(ZBX_API_ERROR_PARAMETERS, _s('Template "%s" already exists.', $template['host']));
				}

				if (API::Host()->exists(array('host' => $template['host']))) {
					self::exception(ZBX_API_ERROR_PARAMETERS, _s('Host "%s" already exists.', $template['host']));
				}
			}

			if (isset($template['name'])) {
				if ($this->exists(array('name' => $template['name']))) {
					self::exception(ZBX_API_ERROR_PARAMETERS, _s('Template with the same visible name "%s" already exists.', $template['name']));
				}

				if (API::Host()->exists(array('name' => $template['name']))) {
					self::exception(ZBX_API_ERROR_PARAMETERS, _s('Host with the same visible name "%s" already exists.', $template['name']));
				}
			}

			$templateid = DB::insert('hosts', array(array('host' => $template['host'],'name' => $template['name'], 'status' => HOST_STATUS_TEMPLATE,)));
			$templateids[] = $templateid = reset($templateid);


			foreach ($template['groups'] as $group) {
				$hostgroupid = get_dbid('hosts_groups', 'hostgroupid');
				$result = DBexecute("INSERT INTO hosts_groups (hostgroupid, hostid, groupid) VALUES ($hostgroupid, $templateid, {$group['groupid']})");
				if (!$result) {
					self::exception(ZBX_API_ERROR_PARAMETERS, 'DBerror');
				}
			}

			$template['templateid'] = $templateid;
			$options = array();
			$options['templates'] = $template;
			if (isset($template['templates']) && !is_null($template['templates']))
				$options['templates_link'] = $template['templates'];
			if (isset($template['macros']) && !is_null($template['macros']))
				$options['macros'] = $template['macros'];
			if (isset($template['hosts']) && !is_null($template['hosts']))
				$options['hosts'] = $template['hosts'];

			$result = $this->massAdd($options);
			if (!$result) self::exception(ZBX_API_ERROR_PARAMETERS);
		}

		return array('templateids' => $templateids);
	}

	/**
	 * Update Template
	 *
	 * @param array $templates multidimensional array with templates data
	 * @return boolean
	 */
	public function update($templates) {
		$templates = zbx_toArray($templates);
		$templateids = zbx_objectValues($templates, 'templateid');

		$updTemplates = $this->get(array(
			'templateids' => $templateids,
			'editable' => 1,
			'output' => API_OUTPUT_EXTEND,
			'preservekeys' => 1
		));

		foreach ($templates as $template) {
			if (!isset($updTemplates[$template['templateid']])) {
				self::exception(ZBX_API_ERROR_PERMISSIONS, _('You do not have permission to perform this operation.'));
			}
		}

		$macros = array();
		foreach ($templates as $template) {
			// if visible name is not given or empty it should be set to host name
			if (!isset($template['name']) || (isset($template['name']) && zbx_empty(trim($template['name']))))
			{
				if (isset($template['host'])) $template['name'] = $template['host'];
			}
			$tplTmp = $template;

			$template['templates_link'] = isset($template['templates']) ? $template['templates'] : null;

			if (isset($template['macros'])) {
				$macros[$template['templateid']] = $template['macros'];
				unset($template['macros']);
			}

			unset($template['templates']);
			unset($template['templateid']);
			unset($tplTmp['templates']);

			$template['templates'] = array($tplTmp);
			$result = $this->massUpdate($template);
			if (!$result) self::exception(ZBX_API_ERROR_PARAMETERS, _('Failed to update template'));
		}

		if ($macros) {
			API::UserMacro()->replaceMacros($macros);
		}

		return array('templateids' => $templateids);
	}

	/**
	 * Delete Template
	 *
	 * @param array $templateids
	 * @param array $templateids['templateids']
	 * @return boolean
	 */
	public function delete($templateids) {

		if (empty($templateids)) {
			self::exception(ZBX_API_ERROR_PARAMETERS, _('Empty input parameter.'));
		}

		$templateids = zbx_toArray($templateids);

		$options = array(
			'templateids' => $templateids,
			'editable' => true,
			'output' => API_OUTPUT_EXTEND,
			'preservekeys' => true
		);
		$delTemplates = $this->get($options);
		foreach ($templateids as $templateid) {
			if (!isset($delTemplates[$templateid])) {
				self::exception(ZBX_API_ERROR_PERMISSIONS, _('You do not have permission to perform this operation.'));
			}
		}

		API::Template()->unlink($templateids, null, true);

		// delete the discovery rules first
		$delRules = API::DiscoveryRule()->get(array(
			'hostids' => $templateids,
			'nopermissions' => true,
			'preservekeys' => true
		));
		if ($delRules) {
			API::DiscoveryRule()->delete(array_keys($delRules), true);
		}

		// delete the items
		$delItems = API::Item()->get(array(
			'templateids' => $templateids,
			'filter' => array('flags' => array(ZBX_FLAG_DISCOVERY_NORMAL, ZBX_FLAG_DISCOVERY_CREATED)),
			'output' => API_OUTPUT_SHORTEN,
			'nopermissions' => true,
			'preservekeys' => true
		));
		if ($delItems) {
			API::Item()->delete(array_keys($delItems), true);
		}

		// delete screen items
		DBexecute('DELETE FROM screens_items WHERE '.DBcondition('resourceid', $templateids).' AND resourcetype='.SCREEN_RESOURCE_HOST_TRIGGERS);

		// delete host from maps
		if (!empty($templateids)) {
			DB::delete('sysmaps_elements', array('elementtype' => SYSMAP_ELEMENT_TYPE_HOST, 'elementid' => $templateids));
		}

		// disable actions
		// actions from conditions
		$actionids = array();
		$sql = 'SELECT DISTINCT actionid'.
			' FROM conditions'.
			' WHERE conditiontype='.CONDITION_TYPE_HOST.
			' AND '.DBcondition('value', $templateids);
		$dbActions = DBselect($sql);
		while ($dbAction = DBfetch($dbActions)) {
			$actionids[$dbAction['actionid']] = $dbAction['actionid'];
		}

		// actions from operations
		$sql = 'SELECT DISTINCT o.actionid'.
			' FROM operations o,optemplate ot'.
			' WHERE o.operationid=ot.operationid'.
			' AND '.DBcondition('ot.templateid', $templateids);
		$dbActions = DBselect($sql);
		while ($dbAction = DBfetch($dbActions)) {
			$actionids[$dbAction['actionid']] = $dbAction['actionid'];
		}

		if (!empty($actionids)) {
			DB::update('actions', array(
				'values' => array('status' => ACTION_STATUS_DISABLED),
				'where' => array('actionid' => $actionids)
			));
		}

		// delete action conditions
		DB::delete('conditions', array(
			'conditiontype'=>CONDITION_TYPE_HOST,
			'value'=>$templateids
		));

		// delete action operation commands
		$operationids = array();
		$sql = 'SELECT DISTINCT ot.operationid'.
			' FROM optemplate ot'.
			' WHERE '.DBcondition('ot.templateid', $templateids);
		$dbOperations = DBselect($sql);
		while ($dbOperation = DBfetch($dbOperations)) {
			$operationids[$dbOperation['operationid']] = $dbOperation['operationid'];
		}

		DB::delete('optemplate', array(
			'templateid'=>$templateids,
		));

		// delete empty operations
		$delOperationids = array();
		$sql = 'SELECT DISTINCT o.operationid'.
			' FROM operations o'.
			' WHERE '.DBcondition('o.operationid', $operationids).
			' AND NOT EXISTS(SELECT ot.optemplateid FROM optemplate ot WHERE ot.operationid=o.operationid)';
		$dbOperations = DBselect($sql);
		while ($dbOperation = DBfetch($dbOperations)) {
			$delOperationids[$dbOperation['operationid']] = $dbOperation['operationid'];
		}

		DB::delete('operations', array(
			'operationid'=>$delOperationids,
		));

		// Applications
		$delApplications = API::Application()->get(array(
			'templateids' => $templateids,
			'output' => API_OUTPUT_SHORTEN,
			'nopermissions' => 1,
			'preservekeys' => 1
		));
		if (!empty($delApplications)) {
			API::Application()->delete(array_keys($delApplications), true);
		}


		DB::delete('hosts', array('hostid' => $templateids));

		// TODO: remove info from API
		foreach ($delTemplates as $template) {
			info(_s('Deleted: Template "%1$s".', $template['name']));
			add_audit_ext(AUDIT_ACTION_DELETE, AUDIT_RESOURCE_HOST, $template['hostid'], $template['host'], 'hosts', NULL, NULL);
		}

		return array('templateids' => $templateids);
	}

	/**
	 * Link Template to Hosts
	 *
	 * @param array $data
	 * @param string $data['templates']
	 * @param string $data['hosts']
	 * @param string $data['groups']
	 * @param string $data['templates_link']
	 * @return boolean
	 */
	public function massAdd($data) {
		$templates = isset($data['templates']) ? zbx_toArray($data['templates']) : null;
		$templateids = is_null($templates) ? array() : zbx_objectValues($templates, 'templateid');

		$updTemplates = $this->get(array(
			'templateids' => $templateids,
			'editable' => 1,
			'preservekeys' => 1
		));

		foreach ($templates as $template) {
			if (!isset($updTemplates[$template['templateid']])) {
				self::exception(ZBX_API_ERROR_PERMISSIONS, _('You do not have permission to perform this operation.'));
			}
		}

		if (isset($data['groups']) && !empty($data['groups'])) {
			$options = array('groups' => $data['groups'], 'templates' => $templates);
			$result = API::HostGroup()->massAdd($options);
			if (!$result) self::exception(ZBX_API_ERROR_PARAMETERS, 'Can\'t link groups');
		}

		if (isset($data['hosts']) && !empty($data['hosts'])) {
			$hostids = zbx_objectValues($data['hosts'], 'hostid');
			$this->link($templateids, $hostids);
		}

		if (isset($data['templates_link']) && !empty($data['templates_link'])) {
			$templatesLinkids = zbx_objectValues($data['templates_link'], 'templateid');
			$this->link($templatesLinkids, $templateids);
		}

		if (isset($data['macros']) && !empty($data['macros'])) {
			$data['macros'] = zbx_toArray($data['macros']);

			// add the macros to all hosts
			$hostMacrosToAdd = array();
			foreach ($data['macros'] as $hostMacro) {
				foreach ($templateids as $templateId) {
					$hostMacro['hostid'] = $templateId;
					$hostMacrosToAdd[] = $hostMacro;
				}
			}
			$result = API::UserMacro()->create($hostMacrosToAdd);
			if (!$result) self::exception(ZBX_API_ERROR_PARAMETERS, 'Can\'t link macros');
		}

		return array('templateids' => zbx_objectValues($data['templates'], 'templateid'));
	}

	/**
	 * Mass update hosts
	 *
	 * @param _array $hosts multidimensional array with Hosts data
	 * @param array $hosts['hosts'] Array of Host objects to update
	 * @return boolean
	 */
	public function massUpdate($data) {
		$templates = zbx_toArray($data['templates']);
		$templateids = zbx_objectValues($templates, 'templateid');

		$options = array(
			'templateids' => $templateids,
			'editable' => true,
			'output' => API_OUTPUT_EXTEND,
			'preservekeys' => true,
		);
		$updTemplates = $this->get($options);
		foreach ($templates as $tnum => $template) {
			if (!isset($updTemplates[$template['templateid']])) {
				self::exception(ZBX_API_ERROR_PERMISSIONS, _('You do not have permission to perform this operation.'));
			}
		}

		// CHECK IF TEMPLATES HAVE AT LEAST 1 GROUP {{{
		if (isset($data['groups']) && empty($data['groups'])) {
			self::exception(ZBX_API_ERROR_PARAMETERS, _('No groups for template'));
		}
		// }}} CHECK IF TEMPLATES HAVE AT LEAST 1 GROUP


		// UPDATE TEMPLATES PROPERTIES {{{
		if (isset($data['name'])) {
			if (count($templates) > 1) {
				self::exception(ZBX_API_ERROR_PARAMETERS, _('Cannot mass update visible template name'));
			}

			$curTemplate = reset($templates);

			$options = array(
				'filter' => array(
					'name' => $curTemplate['name']),
				'output' => API_OUTPUT_SHORTEN,
				'editable' => 1,
				'nopermissions' => 1
			);
			$templateExists = $this->get($options);
			$templateExist = reset($templateExists);

			if ($templateExist && (bccomp($templateExist['templateid'], $curTemplate['templateid']) != 0)) {
				self::exception(ZBX_API_ERROR_PARAMETERS, _s('Template with the same visible name "%s" already exists.', $curTemplate['name']));
			}

			// can't set the same name as existing host
			if (API::Host()->exists(array('name' => $curTemplate['name']))) {
				self::exception(ZBX_API_ERROR_PARAMETERS, _s('Host with the same visible name "%s" already exists.', $curTemplate['name']));
			}
		}

		if (isset($data['host'])) {
			if (count($templates) > 1) {
				self::exception(ZBX_API_ERROR_PARAMETERS, _('Cannot mass update template name'));
			}

			$curTemplate = reset($templates);

			$options = array(
				'filter' => array(
					'host' => $curTemplate['host']),
				'output' => API_OUTPUT_SHORTEN,
				'editable' => 1,
				'nopermissions' => 1
			);
			$templateExists = $this->get($options);
			$templateExist = reset($templateExists);

			if ($templateExist && (bccomp($templateExist['templateid'], $curTemplate['templateid']) != 0)) {
				self::exception(ZBX_API_ERROR_PARAMETERS, _s('Template with the same name "%s" already exists.', $curTemplate['host']));
			}

			// can't set the same name as existing host
			if (API::Host()->exists(array('host' => $curTemplate['host']))) {
				self::exception(ZBX_API_ERROR_PARAMETERS, _s('Host with the same name "%s" already exists.', $curTemplate['host']));
			}
		}

		if (isset($data['host']) && !preg_match('/^'.ZBX_PREG_HOST_FORMAT.'$/', $data['host'])) {
			self::exception(ZBX_API_ERROR_PARAMETERS, _s('Incorrect characters used for template name "%s".', $data['host']));
		}

		$sqlSet = array();
		if (isset($data['host'])) $sqlSet[] = 'host=' . zbx_dbstr($data['host']);
		if (isset($data['name']))
		{
			// if visible name is empty replace it with host name
			if (zbx_empty(trim($data['name'])) && isset($data['host']))
			{
				$sqlSet[] = 'name=' . zbx_dbstr($data['host']);
			}
// we cannot have empty visible name
			elseif (zbx_empty(trim($data['name'])) && !isset($data['host']))
			{
				self::exception(ZBX_API_ERROR_PARAMETERS, _s('Cannot have empty visible template name'));
			}
			else
			{
				$sqlSet[] = 'name=' . zbx_dbstr($data['name']);
			}
		}

		if (!empty($sqlSet)) {
			$sql = 'UPDATE hosts SET ' . implode(', ', $sqlSet) . ' WHERE ' . DBcondition('hostid', $templateids);
			$result = DBexecute($sql);
		}
		// }}} UPDATE TEMPLATES PROPERTIES


		// UPDATE HOSTGROUPS LINKAGE {{{
		if (isset($data['groups']) && !is_null($data['groups'])) {
			$data['groups'] = zbx_toArray($data['groups']);
			$templateGroups = API::HostGroup()->get(array('hostids' => $templateids));
			$templateGroupids = zbx_objectValues($templateGroups, 'groupid');
			$newGroupids = zbx_objectValues($data['groups'], 'groupid');

			$groupsToAdd = array_diff($newGroupids, $templateGroupids);

			if (!empty($groupsToAdd)) {
				$result = $this->massAdd(array(
					'templates' => $templates,
					'groups' => zbx_toObject($groupsToAdd, 'groupid')
				));
				if (!$result) {
					self::exception(ZBX_API_ERROR_PARAMETERS, _("Can't add group"));
				}
			}

			$groupidsToDel = array_diff($templateGroupids, $newGroupids);
			if (!empty($groupidsToDel)) {
				$result = $this->massRemove(array(
					'templateids' => $templateids,
					'groupids' => $groupidsToDel
				));
				if (!$result) {
					self::exception(ZBX_API_ERROR_PARAMETERS, _("Can't remove group"));
				}
			}
		}
		// }}} UPDATE HOSTGROUPS LINKAGE

		$data['templates_clear'] = isset($data['templates_clear']) ? zbx_toArray($data['templates_clear']) : array();
		$templateidsClear = zbx_objectValues($data['templates_clear'], 'templateid');

		if (!empty($data['templates_clear'])) {
			$result = $this->massRemove(array(
				'templateids' => $templateids,
				'templateids_clear' => $templateidsClear,
			));
		}

		// UPDATE TEMPLATE LINKAGE {{{
		// firstly need to unlink all things, to correctly check circulars

		if (isset($data['hosts']) && !is_null($data['hosts'])) {
			$templateHosts = API::Host()->get(array(
				'templateids' => $templateids,
				'templated_hosts' => 1
			));
			$templateHostids = zbx_objectValues($templateHosts, 'hostid');
			$newHostids = zbx_objectValues($data['hosts'], 'hostid');

			$hostsToDel = array_diff($templateHostids, $newHostids);
			$hostidsToDel = array_diff($hostsToDel, $templateidsClear);

			if (!empty($hostidsToDel)) {
				$result = $this->massRemove(array(
					'hostids' => $hostidsToDel,
					'templateids' => $templateids
				));
				if (!$result) {
					self::exception(ZBX_API_ERROR_PARAMETERS, _("Can't unlink template"));
				}
			}
		}

		if (isset($data['templates_link']) && !is_null($data['templates_link'])) {
			$templateTemplates = API::Template()->get(array('hostids' => $templateids));
			$templateTemplateids = zbx_objectValues($templateTemplates, 'templateid');
			$newTemplateids = zbx_objectValues($data['templates_link'], 'templateid');

			$templatesToDel = array_diff($templateTemplateids, $newTemplateids);
			$templateidsToDel = array_diff($templatesToDel, $templateidsClear);
			if (!empty($templateidsToDel)) {
				$result = $this->massRemove(array(
					'templateids' => $templateids,
					'templateids_link' => $templateidsToDel
				));
				if (!$result) {
					self::exception(ZBX_API_ERROR_PARAMETERS, _("Can't unlink template"));
				}
			}
		}

		if (isset($data['hosts']) && !is_null($data['hosts'])) {

			$hostsToAdd = array_diff($newHostids, $templateHostids);
			if (!empty($hostsToAdd)) {
				$result = $this->massAdd(array('templates' => $templates, 'hosts' => $hostsToAdd));
				if (!$result) {
					self::exception(ZBX_API_ERROR_PARAMETERS, _("Can't link template"));
				}
			}
		}

		if (isset($data['templates_link']) && !is_null($data['templates_link'])) {
			$templatesToAdd = array_diff($newTemplateids, $templateTemplateids);
			if (!empty($templatesToAdd)) {
				$result = $this->massAdd(array('templates' => $templates, 'templates_link' => $templatesToAdd));
				if (!$result) {
					self::exception(ZBX_API_ERROR_PARAMETERS, _("Can't link template"));
				}
			}
		}
		// }}} UPDATE TEMPLATE LINKAGE

		// macros
		if (isset($data['macros'])) {
			DB::delete('hostmacro', array('hostid' => $templateids));

			$this->massAdd(array(
				'hosts' => $templates,
				'macros' => $data['macros']
			));
		}

		return array('templateids' => $templateids);
	}

	/**
	 * remove Hosts to HostGroups. All Hosts are added to all HostGroups.
	 *
	 * @param array $data
	 * @param array $data['templateids']
	 * @param array $data['groupids']
	 * @param array $data['hostids']
	 * @param array $data['macroids']
	 * @return boolean
	 */
	public function massRemove($data) {
		$templateids = zbx_toArray($data['templateids']);

		$updTemplates = API::Host()->get(array(
			'hostids' => $templateids,
			'editable' => 1,
			'preservekeys' => 1,
			'templated_hosts' => true,
		));

		foreach ($templateids as $templateid) {
			if (!isset($updTemplates[$templateid])) {
				self::exception(ZBX_API_ERROR_PERMISSIONS, _('You do not have permission to perform this operation.'));
			}
		}

		if (isset($data['groupids'])) {
			$options = array(
				'groupids' => zbx_toArray($data['groupids']),
				'templateids' => $templateids
			);
			$result = API::HostGroup()->massRemove($options);
			if (!$result) self::exception(ZBX_API_ERROR_PARAMETERS, _("Can't unlink groups"));
		}

		if (isset($data['templateids_clear'])) {
			$templateidsClear = zbx_toArray($data['templateids_clear']);
			$result = API::Template()->unlink($templateidsClear, $data['templateids'], true);
		}

		if (isset($data['hostids'])) {
			$hostids = zbx_toArray($data['hostids']);
			$result = API::Template()->unlink($templateids, $hostids);
		}

		if (isset($data['templateids_link'])) {
			$templateidsLink = zbx_toArray($data['templateids_link']);
			$result = API::Template()->unlink($templateidsLink, $templateids);
		}

		if (isset($data['macros'])) {
			$result = API::UserMacro()->delete(zbx_toArray($data['macros']));
			if (!$result) self::exception(ZBX_API_ERROR_PARAMETERS, _("Can't remove macros"));
		}

		return array('templateids' => $templateids);
	}

	private function link($templateids, $targetids) {
		if (empty($templateids)) return true;

		//check if someone passed duplicate templates in the same query
		$templateIdDuplicates = zbx_arrayFindDuplicates($templateids);
		if (!zbx_empty($templateIdDuplicates)) {
			$duplicatesFound = array();
			foreach ($templateIdDuplicates as $value => $count) {
				$duplicatesFound[] = _s('template ID "%1$s" is passed %2$s times', $value, $count);
			}
			self::exception(
				ZBX_API_ERROR_PARAMETERS,
				_s('Cannot pass duplicate template IDs for the linkage: %s.', implode(", ", $duplicatesFound))
			);
		}


		// check if any templates linked to targets have more than one unique item key/application
		foreach ($targetids as $targetid) {
			$linkedTpls = $this->get(array(
				'nopermissions' => 1,
				'output' => API_OUTPUT_SHORTEN,
				'hostids' => $targetid
			));
			$allids = array_merge($templateids, zbx_objectValues($linkedTpls, 'templateid'));

			$sql = 'SELECT key_,count(itemid) as cnt'.
				' FROM items'.
				' WHERE'.DBcondition('hostid', $allids).
				' GROUP BY key_'.
				' HAVING count(itemid)>1';
			$res = DBselect($sql);
			if ($dbCnt = DBfetch($res)) {
				self::exception(ZBX_API_ERROR_PARAMETERS,
					_s('Template with item key "%1$s" already linked to host.', htmlspecialchars($dbCnt['key_'])));
			}

			$sql = 'SELECT name,count(applicationid) as cnt'.
				' FROM applications'.
				' WHERE '.DBcondition('hostid', $allids).
				' GROUP BY name'.
				' HAVING count(applicationid)>1';
			$res = DBselect($sql);
			if ($dbCnt = DBfetch($res)) {
				self::exception(ZBX_API_ERROR_PARAMETERS,
					_s('Template with application "%1$s" already linked to host.', htmlspecialchars($dbCnt['name'])));
			}
		}

		// CHECK TEMPLATE TRIGGERS DEPENDENCIES
		foreach ($templateids as $templateid) {
			$triggerids = array();
			$dbTriggers = get_triggers_by_hostid($templateid);
			while ($trigger = DBfetch($dbTriggers)) {
				$triggerids[$trigger['triggerid']] = $trigger['triggerid'];
			}

			$sql = 'SELECT DISTINCT h.host'.
				' FROM trigger_depends td,functions f,items i,hosts h'.
				' WHERE ('.
				DBcondition('td.triggerid_down', $triggerids).
				' AND f.triggerid=td.triggerid_up'.
				' )'.
				' AND i.itemid=f.itemid'.
				' AND h.hostid=i.hostid'.
				' AND '.DBcondition('h.hostid', $templateids, true).
				' AND h.status='.HOST_STATUS_TEMPLATE;

			if ($dbDepHost = DBfetch(DBselect($sql))) {
				$options = array(
					'templateids' => $templateid,
					'output'=> API_OUTPUT_EXTEND
				);

				$tmpTpls = $this->get($options);
				$tmpTpl = reset($tmpTpls);

				self::exception(ZBX_API_ERROR_PARAMETERS,
					_s('Trigger in template "%1$s" has dependency with trigger in template "%2$s".', $tmpTpl['host'], $dbDepHost['host']));
			}
		}


		$linked = array();
		$sql = 'SELECT hostid,templateid'.
			' FROM hosts_templates'.
			' WHERE '.DBcondition('hostid', $targetids).
			' AND '.DBcondition('templateid', $templateids);
		$linkedDb = DBselect($sql);
		while ($pair = DBfetch($linkedDb)) {
			$linked[] = array($pair['hostid'] => $pair['templateid']);
		}

		// add template linkages, if problems rollback later
		foreach ($targetids as $targetid) {
			foreach ($templateids as $templateid) {
				foreach ($linked as $link) {
					if (isset($link[$targetid]) && bccomp($link[$targetid], $templateid) == 0) {
						continue 2;
					}
				}

				$values = array(get_dbid('hosts_templates', 'hosttemplateid'), $targetid, $templateid);
				$sql = 'INSERT INTO hosts_templates VALUES ('. implode(', ', $values) .')';
				$result = DBexecute($sql);

				if (!$result) {
					self::exception(ZBX_API_ERROR_PARAMETERS, 'DBError');
				}
			}
		}

		// check if all trigger templates are linked to host.
		// we try to find template that is not linked to hosts ($targetids)
		// and exists trigger which reference that template and template from ($templateids)
		$sql = 'SELECT DISTINCT h.host'.
			' FROM functions f,items i,triggers t,hosts h'.
			' WHERE f.itemid=i.itemid'.
			' AND f.triggerid=t.triggerid'.
			' AND i.hostid=h.hostid'.
			' AND h.status='.HOST_STATUS_TEMPLATE.
			' AND NOT EXISTS ('.
			'SELECT 1'.
			' FROM hosts_templates ht'.
			' WHERE ht.templateid=i.hostid'.
			' AND '.DBcondition('ht.hostid', $targetids).
			')'.
			' AND EXISTS ('.
			'SELECT 1'.
			' FROM functions ff,items ii'.
			' WHERE ff.itemid=ii.itemid'.
			' AND ff.triggerid=t.triggerid'.
			' AND '.DBcondition('ii.hostid', $templateids).
			')';
		if ($dbNotLinkedTpl = DBfetch(DBSelect($sql, 1))) {
			self::exception(
				ZBX_API_ERROR_PARAMETERS,
				_s('Trigger has items from template "%1$s" that is not linked to host.', $dbNotLinkedTpl['host'])
			);
		}

		// CHECK CIRCULAR LINKAGE
		// get template linkage graph
		$graph = array();
		$sql = 'SELECT ht.hostid,ht.templateid'.
			' FROM hosts_templates ht,hosts h'.
			' WHERE ht.hostid=h.hostid'.
			' AND h.status='.HOST_STATUS_TEMPLATE;
		$dbGraph = DBselect($sql);
		while ($branch = DBfetch($dbGraph)) {
			if (!isset($graph[$branch['hostid']])) $graph[$branch['hostid']] = array();
			$graph[$branch['hostid']][$branch['templateid']] = $branch['templateid'];
		}

		// get points that have more than one parent templates
		$startPoints = array();
		$sql = 'SELECT max(ht.hostid) as hostid,ht.templateid'.
			' FROM('.
			'SELECT count(htt.templateid) as ccc,htt.hostid'.
			' FROM hosts_templates htt'.
			' WHERE htt.hostid NOT IN (SELECT httt.templateid FROM hosts_templates httt)'.
			' GROUP BY htt.hostid'.
			') ggg, hosts_templates ht'.
			' WHERE ggg.ccc>1'.
			' AND ht.hostid=ggg.hostid'.
			' GROUP BY ht.templateid';
		$dbStartPoints = DBselect($sql);
		while ($startPoint = DBfetch($dbStartPoints)) {
			$startPoints[] = $startPoint['hostid'];
			$graph[$startPoint['hostid']][$startPoint['templateid']] = $startPoint['templateid'];
		}

		// add to the start points also points which we add current templates
		$startPoints = array_merge($startPoints, $targetids);
		$startPoints = array_unique($startPoints);

		foreach ($startPoints as $start) {
			$path = array();
			if (!$this->checkCircularLink($graph, $start, $path)) {
				self::exception(ZBX_API_ERROR_PARAMETERS, _('Circular link cannot be created'));
			}
		}

		foreach ($targetids as $targetid) {
			foreach ($templateids as $templateid) {
				foreach ($linked as $link) {
					if (isset($link[$targetid]) && bccomp($link[$targetid], $templateid) == 0) {
						continue 2;
					}
				}

				API::Application()->syncTemplates(array(
					'hostids' => $targetid,
					'templateids' => $templateid
				));

				API::DiscoveryRule()->syncTemplates(array(
					'hostids' => $targetid,
					'templateids' => $templateid
				));

				API::Itemprototype()->syncTemplates(array(
					'hostids' => $targetid,
					'templateids' => $templateid
				));

				API::Item()->syncTemplates(array(
					'hostids' => $targetid,
					'templateids' => $templateid
				));
			}

			// we do linkage in two separate loops because for triggers you need all items already created on host
			foreach ($templateids as $templateid) {
				foreach ($linked as $link) {
					if (isset($link[$targetid]) && bccomp($link[$targetid], $templateid) == 0) {
						continue 2;
					}
				}

				// sync triggers
				API::Trigger()->syncTemplates(array(
					'hostids' => $targetid,
					'templateids' => $templateid
				));

				API::TriggerPrototype()->syncTemplates(array(
					'hostids' => $targetid,
					'templateids' => $templateid
				));

				API::GraphPrototype()->syncTemplates(array(
					'hostids' => $targetid,
					'templateids' => $templateid
				));

				API::Graph()->syncTemplates(array(
					'hostids' => $targetid,
					'templateids' => $templateid
				));
			}
		}
		API::Trigger()->syncTemplateDependencies(array(
			'templateids' => $templateids,
			'hostids' => $targetids,
		));

		return true;
	}

	private function unlink($templateids, $targetids=null, $clear=false) {

		if ($clear) {
			$flags = array(ZBX_FLAG_DISCOVERY_NORMAL, ZBX_FLAG_DISCOVERY);
		}
		else{
			$flags = array(ZBX_FLAG_DISCOVERY_NORMAL, ZBX_FLAG_DISCOVERY, ZBX_FLAG_DISCOVERY_CHILD);
		}

		/* TRIGGERS {{{ */
		// check that all triggers on templates that we unlink, don't have items from another templates
		$sql = 'SELECT DISTINCT t.description'.
			' FROM triggers t,functions f,items i'.
			' WHERE t.triggerid=f.triggerid'.
			' AND f.itemid=i.itemid'.
			' AND '.DBCondition('i.hostid', $templateids).
			' AND EXISTS ('.
			'SELECT ff.triggerid'.
			' FROM functions ff,items ii'.
			' WHERE ff.itemid=ii.itemid'.
			' AND ff.triggerid=t.triggerid'.
			' AND '.DBCondition('ii.hostid', $templateids, true).
			')'.
			' AND t.flags='.ZBX_FLAG_DISCOVERY_NORMAL;
		if ($dbTrigger = DBfetch(DBSelect($sql, 1))) {
			self::exception(
				ZBX_API_ERROR_PARAMETERS,
				_s('Cannot unlink trigger "%s", it has items from template that is left linked to host.', $dbTrigger['description'])
			);
		}


		$sqlFrom = ' triggers t,hosts h';
		$sqlWhere = ' EXISTS ('.
			'SELECT ff.triggerid'.
			' FROM functions ff,items ii'.
			' WHERE ff.triggerid=t.templateid'.
			' AND ii.itemid=ff.itemid'.
			' AND '.DBCondition('ii.hostid', $templateids).')'.
			' AND '.DBCondition('t.flags', $flags);


		if (!is_null($targetids)) {
			$sqlFrom = ' triggers t,functions f,items i,hosts h';
			$sqlWhere .= ' AND '.DBCondition('i.hostid', $targetids).
				' AND f.itemid=i.itemid'.
				' AND t.triggerid=f.triggerid'.
				' AND h.hostid=i.hostid';
		}
		$sql = 'SELECT DISTINCT t.triggerid,t.description,t.flags,t.expression,h.name as host'.
			' FROM '.$sqlFrom.
			' WHERE '.$sqlWhere;
		$dbTriggers = DBSelect($sql);
		$triggers = array(
			ZBX_FLAG_DISCOVERY_NORMAL => array(),
			ZBX_FLAG_DISCOVERY_CHILD => array()
		);
		$triggerids = array();
		while ($trigger = DBfetch($dbTriggers)) {
			$triggers[$trigger['flags']][$trigger['triggerid']] = array(
				'description' => $trigger['description'],
				'expression' => explode_exp($trigger['expression']),
				'triggerid' => $trigger['triggerid'],
				'host' => $trigger['host']
			);
			if (!in_array($trigger['triggerid'], $triggerids)) {
				array_push($triggerids, $trigger['triggerid']);
			}
		}

		if (!empty($triggers[ZBX_FLAG_DISCOVERY_NORMAL])) {
			if ($clear) {
				$result = API::Trigger()->delete(array_keys($triggers[ZBX_FLAG_DISCOVERY_NORMAL]), true);
				if (!$result) self::exception(ZBX_API_ERROR_INTERNAL, _('Cannot unlink and clear triggers'));
			}
			else{
				DB::update('triggers', array(
					'values' => array('templateid' => 0),
					'where' => array('triggerid' => array_keys($triggers[ZBX_FLAG_DISCOVERY_NORMAL]))
				));

				foreach ($triggers[ZBX_FLAG_DISCOVERY_NORMAL] as $trigger) {
					info(_s('Unlinked: Trigger "%1$s" on "%2$s".', $trigger['description'], $trigger['host']));
				}
			}
		}

		if (!empty($triggers[ZBX_FLAG_DISCOVERY_CHILD])) {
			if ($clear) {
				$result = API::TriggerPrototype()->delete(array_keys($triggers[ZBX_FLAG_DISCOVERY_CHILD]), true);
				if (!$result) self::exception(ZBX_API_ERROR_INTERNAL, _('Cannot unlink and clear triggers'));
			}
			else{
				DB::update('triggers', array(
					'values' => array('templateid' => 0),
					'where' => array('triggerid' => array_keys($triggers[ZBX_FLAG_DISCOVERY_CHILD]))
				));

				foreach ($triggers[ZBX_FLAG_DISCOVERY_CHILD] as $trigger) {
					info(_s('Unlinked: Trigger prototype "%1$s" on "%2$s".', $trigger['description'], $trigger['host']));
				}
			}
		}
		/* }}} TRIGGERS */


		/* ITEMS, DISCOVERY RULES {{{ */
		$sqlFrom = ' items i1,items i2,hosts h';
		$sqlWhere = ' i2.itemid=i1.templateid'.
			' AND '.DBCondition('i2.hostid', $templateids).
			' AND '.DBCondition('i1.flags', $flags).
			' AND h.hostid=i1.hostid';

		if (!is_null($targetids)) {
			$sqlWhere .= ' AND '.DBCondition('i1.hostid', $targetids);
		}
		$sql = 'SELECT DISTINCT i1.itemid,i1.flags,i1.name,i1.hostid,h.name as host'.
			' FROM '.$sqlFrom.
			' WHERE '.$sqlWhere;
		$dbItems = DBSelect($sql);
		$items = array(
			ZBX_FLAG_DISCOVERY_NORMAL => array(),
			ZBX_FLAG_DISCOVERY => array(),
			ZBX_FLAG_DISCOVERY_CHILD => array(),
		);
		while ($item = DBfetch($dbItems)) {
			$items[$item['flags']][$item['itemid']] = array(
				'name' => $item['name'],
				'host' => $item['host']
			);
		}

		if (!empty($items[ZBX_FLAG_DISCOVERY])) {
			if ($clear) {
				$result = API::DiscoveryRule()->delete(array_keys($items[ZBX_FLAG_DISCOVERY]), true);
				if (!$result) self::exception(ZBX_API_ERROR_INTERNAL, _('Cannot unlink and clear discovery rules'));
			}
			else{
				DB::update('items', array(
					'values' => array('templateid' => 0),
					'where' => array('itemid' => array_keys($items[ZBX_FLAG_DISCOVERY]))
				));

				foreach ($items[ZBX_FLAG_DISCOVERY] as $discoveryRule) {
					info(_s('Unlinked: Discovery rule "%1$s" on "%2$s".', $discoveryRule['name'], $discoveryRule['host']));
				}
			}
		}


		if (!empty($items[ZBX_FLAG_DISCOVERY_NORMAL])) {
			if ($clear) {
				$result = API::Item()->delete(array_keys($items[ZBX_FLAG_DISCOVERY_NORMAL]), true);
				if (!$result) self::exception(ZBX_API_ERROR_INTERNAL, _('Cannot unlink and clear items'));
			}
			else{
				DB::update('items', array(
					'values' => array('templateid' => 0),
					'where' => array('itemid' => array_keys($items[ZBX_FLAG_DISCOVERY_NORMAL]))
				));

				foreach ($items[ZBX_FLAG_DISCOVERY_NORMAL] as $item) {
					info(_s('Unlinked: Item "%1$s" on "%2$s".', $item['name'], $item['host']));
				}
			}
		}


		if (!empty($items[ZBX_FLAG_DISCOVERY_CHILD])) {
			if ($clear) {
				$result = API::Itemprototype()->delete(array_keys($items[ZBX_FLAG_DISCOVERY_CHILD]), true);
				if (!$result) self::exception(ZBX_API_ERROR_INTERNAL, _('Cannot unlink and clear item prototypes'));
			}
			else{
				DB::update('items', array(
					'values' => array('templateid' => 0),
					'where' => array('itemid' => array_keys($items[ZBX_FLAG_DISCOVERY_CHILD]))
				));

				foreach ($items[ZBX_FLAG_DISCOVERY_CHILD] as $item) {
					info(_s('Unlinked: Item prototype "%1$s" on "%2$s".', $item['name'], $item['host']));
				}
			}
		}
		/* }}} ITEMS, DISCOVERY RULES */


		/* GRAPHS {{{ */
		$sqlFrom = ' graphs g,hosts h';
		$sqlWhere = ' EXISTS ('.
			'SELECT ggi.graphid'.
			' FROM graphs_items ggi,items ii'.
			' WHERE ggi.graphid=g.templateid'.
			' AND ii.itemid=ggi.itemid'.
			' AND '.DBCondition('ii.hostid', $templateids).')'.
			' AND '.DBCondition('g.flags', $flags);


		if (!is_null($targetids)) {
			$sqlFrom = ' graphs g,graphs_items gi,items i,hosts h';
			$sqlWhere .= ' AND '.DBCondition('i.hostid', $targetids).
				' AND gi.itemid=i.itemid'.
				' AND g.graphid=gi.graphid'.
				' AND h.hostid=i.hostid';
		}
		$sql = 'SELECT DISTINCT g.graphid,g.name,g.flags,h.name as host'.
			' FROM '.$sqlFrom.
			' WHERE '.$sqlWhere;
		$dbGraphs = DBSelect($sql);
		$graphs = array(
			ZBX_FLAG_DISCOVERY_NORMAL => array(),
			ZBX_FLAG_DISCOVERY_CHILD => array(),
		);
		while ($graph = DBfetch($dbGraphs)) {
			$graphs[$graph['flags']][$graph['graphid']] = array(
				'name' => $graph['name'],
				'graphid' => $graph['graphid'],
				'host' => $graph['host']
			);
		}

		if (!empty($graphs[ZBX_FLAG_DISCOVERY_CHILD])) {
			if ($clear) {
				$result = API::GraphPrototype()->delete(array_keys($graphs[ZBX_FLAG_DISCOVERY_CHILD]), true);
				if (!$result) self::exception(ZBX_API_ERROR_INTERNAL, _('Cannot unlink and clear graph prototypes'));
			}
			else{
				DB::update('graphs', array(
					'values' => array('templateid' => 0),
					'where' => array('graphid' => array_keys($graphs[ZBX_FLAG_DISCOVERY_CHILD]))
				));

				foreach ($graphs[ZBX_FLAG_DISCOVERY_CHILD] as $graph) {
					info(_s('Unlinked: Graph prototype "%1$s" on "%2$s".', $graph['name'], $graph['host']));
				}
			}
		}


		if (!empty($graphs[ZBX_FLAG_DISCOVERY_NORMAL])) {
			if ($clear) {
				$result = API::Graph()->delete(array_keys($graphs[ZBX_FLAG_DISCOVERY_NORMAL]), true);
				if (!$result) self::exception(ZBX_API_ERROR_INTERNAL, _('Cannot unlink and clear graphs.'));
			}
			else{
				DB::update('graphs', array(
					'values' => array('templateid' => 0),
					'where' => array('graphid' => array_keys($graphs[ZBX_FLAG_DISCOVERY_NORMAL]))
				));

				foreach ($graphs[ZBX_FLAG_DISCOVERY_NORMAL] as $graph) {
					info(_s('Unlinked: Graph "%1$s" on "%2$s".', $graph['name'], $graph['host']));
				}
			}
		}
		/* }}} GRAPHS */


		/* APPLICATIONS {{{ */
		$sqlFrom = ' applications a1,applications a2,hosts h';
		$sqlWhere = ' a2.applicationid=a1.templateid'.
			' AND '.DBCondition('a2.hostid', $templateids).
			' AND h.hostid=a1.hostid';
		if (!is_null($targetids)) {
			$sqlWhere .= ' AND '.DBCondition('a1.hostid', $targetids);
		}
		$sql = 'SELECT DISTINCT a1.applicationid,a1.name,a1.hostid,h.name as host'.
			' FROM '.$sqlFrom.
			' WHERE '.$sqlWhere;
		$dbApplications = DBSelect($sql);
		$applications = array();
		while ($application = DBfetch($dbApplications)) {
			$applications[$application['applicationid']] = array(
				'name' => $application['name'],
				'hostid' => $application['hostid'],
				'host' => $application['host']
			);
		}

		if (!empty($applications)) {
			if ($clear) {
				$result = API::Application()->delete(array_keys($applications), true);
				if (!$result) self::exception(ZBX_API_ERROR_INTERNAL, _('Cannot unlink and clear applications'));
			}
			else{
				DB::update('applications', array(
					'values' => array('templateid' => 0),
					'where' => array('applicationid' => array_keys($applications))
				));

				foreach ($applications as $application) {
					info(_s('Unlinked: Application "%1$s" on "%2$s".', $application['name'], $application['host']));
				}
			}
		}
		/* }}} APPLICATIONS */


		$cond = array('templateid' => $templateids);
		if (!is_null($targetids)) $cond['hostid'] =  $targetids;
		DB::delete('hosts_templates', $cond);

		if (!is_null($targetids)) {
			$hosts = API::Host()->get(array(
				'hostids' => $targetids,
				'output' => array('hostid', 'host'),
				'nopermissions' => true,
			));
		}
		else{
			$hosts = API::Host()->get(array(
				'templateids' => $templateids,
				'output' => array('hostid', 'host'),
				'nopermissions' => true,
			));
		}

		if (!empty($hosts)) {
			$templates = API::Template()->get(array(
				'templateids' => $templateids,
				'output' => array('hostid', 'host'),
				'nopermissions' => true,
			));

			$hosts = implode(', ', zbx_objectValues($hosts, 'host'));
			$templates = implode(', ', zbx_objectValues($templates, 'host'));

			info(_s('Templates "%1$s" unlinked from hosts "%2$s".', $templates, $hosts));
		}

		return true;
	}

	private function checkCircularLink(&$graph, $current, &$path) {

		if (isset($path[$current])) return false;
		$path[$current] = $current;
		if (!isset($graph[$current])) return true;

		foreach ($graph[$current] as $step) {
			if (!$this->checkCircularLink($graph, $step, $path)) return false;
		}

		return true;
	}

	public function isReadable($ids) {
		if (!is_array($ids)) return false;
		if (empty($ids)) return true;

		$ids = array_unique($ids);

		$count = $this->get(array(
			'nodeids' => get_current_nodeid(true),
			'templateids' => $ids,
			'output' => API_OUTPUT_SHORTEN,
			'countOutput' => true
		));

		return (count($ids) == $count);
	}

	public function isWritable($ids) {
		if (!is_array($ids)) return false;
		if (empty($ids)) return true;

		$ids = array_unique($ids);

		$count = $this->get(array(
			'nodeids' => get_current_nodeid(true),
			'templateids' => $ids,
			'output' => API_OUTPUT_SHORTEN,
			'editable' => true,
			'countOutput' => true
		));

		return (count($ids) == $count);
	}

}
