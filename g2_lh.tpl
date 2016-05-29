<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.0 Transitional//EN" "http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd">
<html xmlns="http://www.w3.org/1999/xhtml" lang="en" xml:lang="en">
<!-- DW6 -->
<head>
<title>{{TITLE}} Dashboard</title>
<meta http-equiv="Content-Type" content="text/html; charset=utf-8" />
<meta http-equiv="content-language" content="en" />
<meta name="google" content="notranslate" />
<link rel="stylesheet" href="g2_ircddb/mm_training.css" type="text/css" />
<style type="text/css">
<!--
.style1 {font-size: 16px}
-->
</style>
</head>
<body bgcolor="#64748B">
<table width="700" border="0" cellspacing="0" cellpadding="0">
  <tr bgcolor="#26354A">
    <td width="15" nowrap="nowrap"><img src="g2_ircddb/mm_spacer.gif" alt="" width="15" height="1" border="0" /></td>
    <td height="70" colspan="2" class="logo" nowrap="nowrap">
      {{TITLE}} Dashboard <span class="tagline">| Gateway Status and Control</span>
    </td>
    <td width="15">&nbsp;</td>
  </tr>
  <tr bgcolor="#FF0000">
    <td colspan="4"><img src="g2_ircddb/mm_spacer.gif" alt="" width="1" height="4" border="0" /></td>
  </tr>
  <tr bgcolor="#D3DCE6">
    <td colspan="4"><img src="g2_ircddb/mm_spacer.gif" alt="" width="1" height="1" border="0" /></td>
  </tr>
  <tr bgcolor="#FFFFFF">
    <td width="15" nowrap="nowrap">&nbsp;</td>
    <td colspan="2" height="24">
      <table width="651" border="0" cellpadding="0" cellspacing="0" id="navigation">
        <tr>
          {{#REG}}<td width="135" align="center" nowrap="nowrap" class="navText">
            <a href="{{REGURL}}"><strong>Registration</strong></a>
          </td>{{/REG}}
          <td width="250" align="center" nowrap="nowrap" class="navText"><strong>{{CALL}} Repeater System</strong></td>
          <td width="230" align="center" nowrap="nowrap" class="navText">
            <strong>Dashboard {{LHVER}}&nbsp;&nbsp;&nbsp;&nbsp;g2_link {{G2VER}}</strong>
          </td>
        </tr>
      </table>
    </td>
    <td width="15">&nbsp;</td>
  </tr>
  <tr bgcolor="#D3DCE6">
    <td colspan="4"><img src="g2_ircddb/mm_spacer.gif" alt="" width="1" height="1" border="0" /></td>
  </tr>
  <tr bgcolor="#FF0000">
    <td colspan="4"><img src="g2_ircddb/mm_spacer.gif" alt="" width="1" height="4" border="0" /></td>
  </tr>
  <tr bgcolor="#D3DCE6">
    <td colspan="4"><img src="g2_ircddb/mm_spacer.gif" alt="" width="1" height="1" border="0" /></td>
  </tr>
  <tr bgcolor="#D3DCE6">
    <td width="15" height="409" valign="top">&nbsp;</td>
    <td width="602" align="center" colspan="2" valign="top"><br />&nbsp;<br />
      <table border="0" cellspacing="0" cellpadding="2" width="500">
        <tr>
          <td height="45" align="center" class="pageName">Linked Gateways / Reflectors</td>
        </tr>
        {{#LINKED}}
        <tr>
          <td>
            <table width="50%" border="1" align="center" cellspacing="2" cellpadding="0">
              <tr bgcolor="#D3DCE6">
                <th width="188" align="center" valign="middle"><span class="style1">Module</span></th>
                <th width="175" align="center" valign="middle"><span class="style1">Linked to</span><br /></th>
              </tr>{{#NODE}}
              <tr bgcolor="#D3DCE6">
                <td width="188" align="center" valign="middle"><span class="style1">{{MODULE}}</span></td>
                <td width="175" align="center" valign="middle"><span class="style1">{{CALL}}</span><br /></td>
              </tr>{{/NODE}}
            </table>
          <br />
          </td>
          </tr>
        {{/LINKED}}
        {{#USERS}}
        <tr>
          <td height="45" align="center" class="pageName">Software Clients</td>
        </tr>
        <tr>
          <td>
            <table width="60%" border="1" align="center" cellspacing="2" cellpadding="0">
              <tr bgcolor="#D3DCE6">
                <th width="15" align="center" valign="middle"><span class="style1">Callsign</span></th>
                <th width="15" align="center" valign="middle"><span class="style1">Module</span></th>
                <th width="15" align="center" valign="middle"><span class="style1">Type</span></th>
              </tr>
              {{#USER}}<tr bgcolor="#D3DCE6">
                <td width="15" align="center" valign="middle"><span class="style1">{{CALL}}</span></td>
                <td width="15" align="center" valign="middle"><span class="style1">{{MODULE}}</span></td>
                <td width="15" align="center" valign="middle"><span class="style1">{{TYPE}}</span></td>
              </tr>{{/USER}}
            </table>
          <br />
          </td>
        </tr>
        {{/USERS}}
        <tr>
          <td height="45" align="center" class="pageName">Local RF Users</td>
        </tr>
        <tr>
          <td>
            <table width="100%" border="1" align="center" cellspacing="2" cellpadding="0">
              <tr bgcolor="#D3DCE6">
                <th width="95" align="center" valign="middle"><span class="style1">Callsign</span></th>
                <th width="63" align="center" valign="middle"><span class="style1">Last TX on</span></th>
                <th width="115" align="center" valign="middle"><span class="style1">Date-Time</span><br /></th>
              </tr>
              {{#LOCAL}}<tr bgcolor="#D3DCE6">
                <td width="95" align="center" valign="middle"><span class="style1">
                  {{#GPS}}<a href="http://aprs.fi/static/a/{{APRSCALL}}"><strong>{{CALL}}</strong></a>{{/GPS}}
                  {{#NOGPS}}{{CALL}}{{/NOGPS}}
                </span></td>
                <td width="63" align="center" valign="middle"><span class="style1">{{TX}}</span></td>
                <td width="115" align="center" valign="middle"><span class="style1">{{TIMESTAMP}}</span><br /></td>
              </tr>{{/LOCAL}}
            </table>
          <br />
          </td>
        </tr>
        <tr>
          <td height="45" align="center" class="pageName">Remote Users</td>
        </tr>
        <tr>
          <td>
            <table width="100%" border="1" align="center" cellspacing="2" cellpadding="0">
              <tr bgcolor="#D3DCE6">
                <th width="70" align="center" valign="middle"><span class="style1">Callsign</span></th>
                <th width="70" align="center" valign="middle"><span class="style1">Last TX on</span></th>
                <th width="70" align="center" valign="middle"><span class="style1">Source</span></th>
                <th width="140" align="center" valign="middle"><span class="style1">Date-Time</span><br /></th>
              </tr>
              {{#REMOTE}}<tr>
                <td width="70" align="center" valign="middle"><span class="style1">{{CALL}}</span></td>
                <td width="70" align="center" valign="middle"><span class="style1">{{TX}}</span></td>
                <td width="70" align="center" valign="middle"><span class="style1">{{SOURCE}}</span></td>
                <td width="140" align="center" valign="middle"><span class="style1">{{TIMESTAMP}}</span><br /></td>
              </tr>{{/REMOTE}}
            </table>
          <br />
          </td>
        </tr>
        <tr>
          <td align="center" class="style1">Status as of {{NOW}}</td>
        </tr>
        {{#UPTIME}}
        <tr>
          <td height="50" align="center" class="style1">
            Server Uptime: {{DAY}} day{{DAYS}}, {{HOUR}} hour{{HOURS}} and {{MINUTE}} minute{{MINUTES}}
          </td>
        </tr>
        {{/UPTIME}}
      </table>
      <br />
      <td>&nbsp;</td>
    </td>
  </tr>
  <tr>
    <td width="15">&nbsp;<br />  &nbsp;<br /> </td>
    <td width="68">&nbsp;</td>
    <td width="602">&nbsp;</td>
    <td width="15">&nbsp;</td>
  </tr>
</table>
</body>
</html>
