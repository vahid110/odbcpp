"""Owned Windows-provider User DSN for an external desktop consumer.

Uses the supported local WDAC/CIM provider rather than a caller-private registry
view. No connection, password, admin token, cloud operation or GUI automation.
"""
import base64
import json
from pathlib import Path
import re

from windows_powerbi_contract import PS as POWERSHELL, OWN_ROOT, AWS_ROOT

DRIVER='ODBCPP Redshift'
DRIVER_PATH=OWN_ROOT/'odbcpp.dll'
SETUP_FIELDS=frozenset(('SERVER','PORT','DATABASE','SSL','SSLCAFILE','LOGINTIMEOUT','QUERYTIMEOUT','LOGLEVEL','LOGFILE'))
# Exact canonical keys are supplied from the pinned checkout, never a new profile.
RUNTIME_FIELDS=frozenset()

SCRIPT=r'''
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$message=Get-Content -LiteralPath '__REQUEST__' -Raw | ConvertFrom-Json
$current=@(Get-OdbcDsn -DsnType User -Platform '64-bit' -ErrorAction Stop | Where-Object { $_.Name -ieq $message.name })
if($current.Count -gt 1) { throw 'Ambiguous provider DSN' }
if($message.action -ceq 'create') {
    if($current.Count -ne 0) { throw 'Existing provider DSN refused' }
    $setupNames=@('SERVER','PORT','DATABASE','SSL','SSLCAFILE','LOGINTIMEOUT','QUERYTIMEOUT','LOGLEVEL','LOGFILE')
    $properties=@($message.fields.PSObject.Properties | Where-Object { $_.Name -cin $setupNames } | ForEach-Object { $_.Name+'='+[string]$_.Value })
    Add-OdbcDsn -Name $message.name -DriverName 'ODBCPP Redshift' -DsnType User -Platform '64-bit' -SetPropertyValue $properties -ErrorAction Stop
    $base=Get-OdbcDsn -Name $message.name -DsnType User -Platform '64-bit' -ErrorAction Stop
    if($base.DriverName -cne 'ODBCPP Redshift' -or $base.Attribute.Count -ne $properties.Count) { throw 'Create base acknowledgement differs' }
    foreach($entry in @($message.fields.PSObject.Properties | Where-Object { $_.Name -cin $setupNames })) {
        if($base.Attribute[$entry.Name] -cne $entry.Value) { throw 'Create base values differ' }
    }
    $sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    if($sid -cne 'S-1-5-21-1833777940-313916754-1955530366-1002') { throw 'Existing local test operator required' }
    # Runtime-only options are valid driver inputs, outside ConfigDSN's closed
    # setup schema. The local registry provider writes only this owned User DSN.
    $key=$sid+'\Software\ODBC\ODBC.INI\'+$message.name
    foreach($entry in @($message.fields.PSObject.Properties | Where-Object { $_.Name -cnotin $setupNames })) {
        $result=Invoke-CimMethod -Namespace 'root/default' -ClassName StdRegProv -MethodName SetStringValue -Arguments @{hDefKey=[uint32]2147483651;sSubKeyName=$key;sValueName=$entry.Name;sValue=[string]$entry.Value} -ErrorAction Stop
        if($result.ReturnValue -ne 0) { throw 'Owned runtime option write failed' }
    }
    $current=@(Get-OdbcDsn -Name $message.name -DsnType User -Platform '64-bit' -ErrorAction Stop)
} elseif($message.action -ceq 'remove') {
    if($current.Count -ne 1 -or $current[0].DriverName -cne 'ODBCPP Redshift') { throw 'Owned provider DSN missing or changed' }
    if($current[0].Attribute.Count -ne @($message.fields.PSObject.Properties).Count) { throw 'Owned provider DSN shape changed' }
    foreach($entry in $message.fields.PSObject.Properties) {
        if($current[0].Attribute[$entry.Name] -cne $entry.Value) { throw 'Owned provider DSN value changed' }
    }
    Remove-OdbcDsn -InputObject $current[0] -Confirm:$false -ErrorAction Stop
    $current=@(Get-OdbcDsn -DsnType User -Platform '64-bit' -ErrorAction Stop | Where-Object { $_.Name -ieq $message.name })
    if($current.Count) { throw 'Provider DSN removal not absent' }
} elseif($message.action -cne 'get') { throw 'Closed provider action required' }
if($current.Count) {
    @{exists=$true;name=$current[0].Name;driver=$current[0].DriverName;platform=$current[0].Platform;fields=$current[0].Attribute} | ConvertTo-Json -Depth 6 -Compress
} else { @{exists=$false} | ConvertTo-Json -Compress }
'''


class ProviderUserDsn:
    def __init__(self,name,directory,tree,*,backend,native_job,driver,canonical_limits):
        global native,job
        native,job=backend,native_job
        expected=('ODBCPP_PBI_RS_20261021_003' if driver['kind']=='own' else 'ODBCPP_AWS2_COMPARE007')
        native.need(name==expected,'closed_provider_fixture_name')
        self.driver=driver
        self.runtime_fields=frozenset(('TRANSPORTMODE','LOGQUERIES',*canonical_limits))
        self.setup_fields=SETUP_FIELDS
        self.script=SCRIPT
        if driver['kind']=='aws2':
            self.setup_fields=frozenset(('HOSTNAME','PORTNUMBER','DATABASE','IAM','AUTOCREATE','SSLMODE',
                'TRUSTSTORE','MIN_TLS','LOGINTIMEOUT','QUERYTIMEOUT','CONNECTIONRETRYCOUNT',
                'DATABASEMETADATACURRENTDBONLY','LOGLEVEL','LOGPATH'))
            self.runtime_fields=frozenset()
            names="@('"+"','".join(sorted(self.setup_fields))+"')"
            self.script=self.script.replace("@('SERVER','PORT','DATABASE','SSL','SSLCAFILE','LOGINTIMEOUT','QUERYTIMEOUT','LOGLEVEL','LOGFILE')",names)
            self.script=self.script.replace('ODBCPP Redshift','Amazon Redshift ODBC Driver (x64)')
        self.NAME=name
        self.directory=Path(directory)
        self.tree=tree
        self.expected=None
        self.created=False
        self.sequence=0
        self.unknown=False

    def _call(self,action,fields=None):
        native.need(action in ('get','create','remove') and not self.unknown,'provider_operation_or_prior_completion_unknown')
        self.sequence+=1
        stem=self.directory/('provider-'+str(self.sequence))
        request=stem.with_suffix('.json')
        self.tree.write(request,native.json_bytes(dict(action=action,name=self.NAME,fields=fields or {})),retire=False)
        held=native.HeldFile(request,private=True)
        receipt={}
        output=stem.with_suffix('.log')
        try:
            script=self.script.replace('__REQUEST__',str(request).replace("'","''"))
            command=[str(POWERSHELL),'-NoLogo','-NoProfile','-NonInteractive','-OutputFormat','Text','-EncodedCommand',base64.b64encode(script.encode('utf-16-le')).decode('ascii')]
            with output.open('xb') as stream:
                code=job.run(command,stream,receipt,timeout=30)
            if receipt.get('timedOut') or not receipt.get('treeEmpty'):
                self.unknown=True
            native.need(code==0 and not self.unknown,'provider_command_failed_or_unknown')
            native.need(output.stat().st_size<=65536,'provider_output_bound')
            return json.loads(output.read_text(encoding='utf-8-sig'))
        except BaseException as error:
            receipt['helper_error_type']=type(error).__name__
            if action in ('create','remove') or not receipt.get('treeEmpty'):
                self.unknown=True
            raise
        finally:
            try:
                receipt['operation']=action
                self.tree.record(stem.with_suffix('.process.json'),receipt)
            finally:
                held.close()

    def prepare(self,values):
        native.need(self.expected is None,'oneuse_provider_DSN')
        native.need(type(values) is dict and all(type(k) is str for k in values),'provider_fields_object')
        folded={k.upper():v for k,v in values.items()}
        native.need(len(folded)==len(values),'folded_duplicate_provider_field')
        native.need(set(folded)<=self.setup_fields|self.runtime_fields|{'DRIVER','UID'},'closed_provider_field_schema_no_aliases')
        native.need('UID' not in folded or folded['UID']=='odbcpp_pilot_test','existing_test_principal_only')
        native.need(Path(folded.get('DRIVER',''))==Path(self.driver['path']),'qualified_provider_driver_only')
        fields={k:v for k,v in folded.items() if k not in ('DRIVER','UID')}
        native.need(all(re.fullmatch('[A-Za-z][A-Za-z0-9_]{0,63}',k) and type(v) is str and 0<len(v)<=2048
            and not any(c in v for c in '\r\n\0') for k,v in fields.items()),'bounded_provider_fields')
        native.need(not self._call('get')['exists'],'provider_fixture_collision')
        self.expected=fields
        self.tree.record(self.directory/'provider-dsn-intent.json',dict(name=self.NAME,driver=self.driver['description'],fields=fields,credentials_persisted=False))
        result=self._call('create',fields)
        self._verify(result)
        self.created=True
        self.tree.record(self.directory/'provider-dsn-ack.json',dict(name=self.NAME,driver=self.driver['description'],fields=fields,creation_acknowledged=True))

    def _verify(self,result):
        native.need(result.get('exists') is True and result['name']==self.NAME
            and result['driver']==self.driver['description'] and result['platform']=='64-bit'
            and {k.upper():v for k,v in result['fields'].items()}==self.expected,'exact_owned_provider_DSN_changed')

    def verify(self):
        native.need(self.expected is not None,'owned_provider_DSN_not_prepared')
        self._verify(self._call('get'))

    def cleanup(self):
        if self.expected is None:
            return
        native.need(self.created and not self.unknown,'provider_creation_or_mutation_not_acknowledged_retain')
        result=self._call('get')
        if result['exists']:
            self._verify(result)
            native.need(self._call('remove',self.expected)=={'exists':False},'provider_retirement_not_absent')
        self.expected=None
        self.created=False
