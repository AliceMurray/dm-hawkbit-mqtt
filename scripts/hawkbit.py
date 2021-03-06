from __future__ import print_function

import argparse
import pprint
import requests
import json
import sys

__version__ = 1.0

user = 'admin'
password = 'admin'
DS_URL_DEFAULT = 'http://localhost:8080/rest/v1/distributionsets'
SM_URL_DEFAULT = 'http://localhost:8080/rest/v1/softwaremodules'


def publish(provider, name, type, version, description, artifact,
            ds_url, sm_url):
    # Publish Software Module
    headers = {'Content-Type': 'application/json',
               'Accept': 'application/json'}
    sm = {'requiredMigrationStep': False,
          'vendor': provider,
          'name': name,
          'type': type,
          'description': description,
          'version': version}
    response = requests.post(sm_url, data=json.dumps([sm]),
                             auth=(user, password), headers=headers)

    if response.status_code != 500:
        response = json.loads(response.content)

        print('Got response from server when posting software module:')
        pprint.pprint(response)

        if 'errorCode' in response:
            print('An error occured at posting software module; stopping.', file=sys.stderr)
            return

        for item in response:
            if 'id' in item:
                id = item['id']

        # Get urls from software module (other than before API version)
        response = requests.get(sm_url + "/" + str(id), data=json.dumps([sm]),
                             auth=(user, password), headers=headers)

        if response.status_code != 500:

            response = json.loads(response.content)
            print('Got response from server when getting software module with id:')
            pprint.pprint(response)

            artifacts_url = None
            self_url = None
            type_url = None
            metadata_url = None

            if 'errorCode' in response:
                print('An error occurred at getting software module with id; stopping.', file=sys.stderr)
                return

            if '_links' in response:
                if 'artifacts' in response['_links']:
                    artifacts_url = response['_links']['artifacts']['href']
                if 'self' in response['_links']:
                    self_url = response['_links']['self']['href']
                if 'type' in response['_links']:
                    type_url = response['_links']['type']['href']
                if 'metadata' in response['_links']:
                    metadata_url = response['_links']['metadata']['href']

            print (artifacts_url, self_url, type_url, metadata_url)
            if None in (artifacts_url, self_url, type_url, metadata_url):
                print("Couldn't parse response", file=sys.stderr)
                return

    # Upload Artifact
    headers = {'Accept': 'application/json'}
    artifacts = {'file': open(artifact, 'rb')}
    response = requests.post(artifacts_url, auth=(user, password),
                             headers=headers, files=artifacts)
    if response.status_code != 500:
        headers = {'Content-Type': 'application/json',
                   'Accept': 'application/json'}
        ds = {'requiredMigrationStep': False,
              'vendor': provider,
              'name': name,
              'type': type,
              'description': description,
              'version': version,
              'modules': [{'id': id}],
              '_links': {'artifacts': artifacts_url,
                         'self': self_url,
                         'type': type_url,
                         'metadata': metadata_url}}
        response = requests.post(ds_url, data=json.dumps([ds]),
                                 auth=(user, password), headers=headers)
        if response.status_code != 500:
            print('Got response from server when posting artifacts:')
            pprint.pprint(json.loads(response.content))


def main():
    description = 'Simple Hawkbit API Wrapper'
    parser = argparse.ArgumentParser(version=__version__,
                                     description=description)
    parser.add_argument('-p', '--provider', help='SW Module Provider',
                        required=True)
    parser.add_argument('-n', '--name', help='Name', required=True)
    parser.add_argument('-t', '--type', help='Name', required=True)
    parser.add_argument('-sv', '--swversion', help='Version', required=True)
    parser.add_argument('-d', '--description', help='Version', required=True)
    parser.add_argument('-f', '--file', help='Version', required=True)
    parser.add_argument('-ds', '--distribution-sets',
                        help='Distribution Sets URL', default=DS_URL_DEFAULT)
    parser.add_argument('-sm', '--software-modules',
                        help='Software Modules URL', default=SM_URL_DEFAULT)
    args = parser.parse_args()
    publish(args.provider, args.name, args.type, args.swversion,
            args.description, args.file, args.distribution_sets,
            args.software_modules)


if __name__ == '__main__':
    main()
